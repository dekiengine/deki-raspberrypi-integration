#include "SdSpiCard.h"

namespace DekiRaspberryPi
{

namespace
{

// Commands, by their number in the SD specification.
constexpr uint8_t kGoIdleState = 0;
constexpr uint8_t kSendIfCond = 8;
constexpr uint8_t kSendCsd = 9;
constexpr uint8_t kStopTransmission = 12;
constexpr uint8_t kSetBlockLength = 16;
constexpr uint8_t kReadSingleBlock = 17;
constexpr uint8_t kReadMultipleBlock = 18;
constexpr uint8_t kWriteSingleBlock = 24;
constexpr uint8_t kWriteMultipleBlock = 25;
constexpr uint8_t kSdSendOpCond = 41;  // an app command: CMD55 goes first
constexpr uint8_t kAppCommandPrefix = 55;
constexpr uint8_t kReadOcr = 58;
constexpr uint8_t kCrcOnOff = 59;

// R1 response bits.
constexpr uint8_t kR1Ready = 0x00;
constexpr uint8_t kR1Idle = 0x01;
constexpr uint8_t kR1IllegalCommand = 0x04;
constexpr uint8_t kR1NoResponse = 0xFF;

// Tokens that start or end a data block.
constexpr uint8_t kStartBlockToken = 0xFE;
constexpr uint8_t kStartMultiWriteToken = 0xFC;
constexpr uint8_t kStopMultiWriteToken = 0xFD;
constexpr uint8_t kDataAccepted = 0x05;
constexpr uint8_t kDataResponseMask = 0x1F;

// CMD8's argument: 2.7-3.6 V and a check pattern the card echoes.
constexpr uint32_t kIfCondArgument = 0x1AA;
constexpr uint32_t kHighCapacitySupport = 0x40000000;
constexpr uint8_t kOcrCardCapacityStatus = 0x40;

constexpr int kGoIdleAttempts = 10;
constexpr int kResponseBytes = 10;
constexpr uint32_t kReadyTimeoutMs = 500;
constexpr uint32_t kInitTimeoutMs = 1000;
constexpr uint32_t kReadTimeoutMs = 300;
constexpr uint32_t kWriteTimeoutMs = 600;

}  // namespace

SdSpiCard::SdSpiCard(ISdSpiTransport& transport)
    : m_Transport(transport)
{
}

uint8_t SdSpiCard::CommandCrc(const uint8_t* frame)
{
    // CRC7 with the polynomial x^7 + x^3 + 1. Bit 7 of `crc` is scratch: it
    // is shifted out on the next step.
    uint8_t crc = 0;
    for (int i = 0; i < 5; ++i)
    {
        uint8_t data = frame[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = static_cast<uint8_t>(crc << 1);
            if (((data ^ crc) & 0x80) != 0)
            {
                crc ^= 0x09;
            }
            data = static_cast<uint8_t>(data << 1);
        }
    }
    return static_cast<uint8_t>((crc << 1) | 1);
}

bool SdSpiCard::Begin(uint32_t clockHz)
{
    End();
    m_LastError = "";

    // At least 74 clocks with chip select high put the card into its native
    // mode, ready for CMD0. Ten 0xFF bytes give 80.
    m_Transport.Deselect();
    m_Transport.SetClock(kInitClockHz);
    m_Transport.Transfer(nullptr, nullptr, 10);

    m_Transport.Select();

    // CMD0 with chip select low switches the card to SPI mode. A card that was
    // in the middle of something can need a few tries.
    uint8_t response = kR1NoResponse;
    for (int attempt = 0; attempt < kGoIdleAttempts && response != kR1Idle; ++attempt)
    {
        response = Command(kGoIdleState, 0);
    }
    if (response != kR1Idle)
    {
        return Fail("no card answered");
    }

    // CMD8 only exists from version 2 of the specification on, so an old card
    // calls it illegal.
    SdCardType type = SdCardType::Sd1;
    response = Command(kSendIfCond, kIfCondArgument);
    if ((response & kR1IllegalCommand) == 0)
    {
        if (response != kR1Idle)
        {
            return Fail("the card did not take CMD8");
        }
        uint8_t echo[4] = {};
        m_Transport.Transfer(nullptr, echo, sizeof(echo));
        if (echo[3] != (kIfCondArgument & 0xFF) || (echo[2] & 0x0F) != (kIfCondArgument >> 8))
        {
            return Fail("the card does not run at 3.3 V");
        }
        type = SdCardType::Sd2;
    }

    // ACMD41 starts the card's initialisation; it answers "idle" until done.
    // The HCS bit tells a version 2 card that we can address SDHC.
    const uint32_t start = m_Transport.Milliseconds();
    const uint32_t opCondArgument = type == SdCardType::Sd2 ? kHighCapacitySupport : 0;
    while (true)
    {
        response = AppCommand(kSdSendOpCond, opCondArgument);
        if (response == kR1Ready)
        {
            break;
        }
        if (response != kR1Idle)
        {
            return Fail("the card refused to start (ACMD41)");
        }
        if (m_Transport.Milliseconds() - start > kInitTimeoutMs)
        {
            return Fail("timed out waiting for the card to start");
        }
    }

    // The OCR's CCS bit says whether the card counts in sectors (SDHC, SDXC)
    // or in bytes (SDSC).
    if (type == SdCardType::Sd2)
    {
        if (Command(kReadOcr, 0) != kR1Ready)
        {
            return Fail("the card did not send its OCR");
        }
        uint8_t ocr[4] = {};
        m_Transport.Transfer(nullptr, ocr, sizeof(ocr));
        if ((ocr[0] & kOcrCardCapacityStatus) != 0)
        {
            type = SdCardType::Sdhc;
        }
    }

    // An SDSC card can use other block lengths; ask for 512 bytes.
    if (type != SdCardType::Sdhc && Command(kSetBlockLength, kSectorSize) != kR1Ready)
    {
        return Fail("the card refused 512-byte blocks");
    }

    // CRC checks are off by default in SPI mode. Say so anyway, for cards
    // that keep them on; the answer does not matter.
    Command(kCrcOnOff, 0);

    if (!ReadCapacity())
    {
        return false;
    }

    Release();
    m_Transport.SetClock(clockHz);
    m_Type = type;
    return true;
}

void SdSpiCard::End()
{
    m_Type = SdCardType::None;
    m_SectorCount = 0;
}

bool SdSpiCard::ReadSectors(uint32_t sector, uint32_t count, uint8_t* out)
{
    if (!IsReady() || out == nullptr)
    {
        m_LastError = "the card is not started";
        return false;
    }
    if (count == 0)
    {
        return true;
    }
    if (sector >= m_SectorCount || count > m_SectorCount - sector)
    {
        m_LastError = "read past the end of the card";
        return false;
    }

    m_Transport.Select();
    if (count == 1)
    {
        if (Command(kReadSingleBlock, Address(sector)) != kR1Ready)
        {
            return Fail("the card refused a read");
        }
        const bool ok = ReadBlock(out, kSectorSize);
        Release();
        return ok;
    }

    if (Command(kReadMultipleBlock, Address(sector)) != kR1Ready)
    {
        return Fail("the card refused a read");
    }
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i)
    {
        ok = ReadBlock(out + i * kSectorSize, kSectorSize);
    }
    // CMD12 ends the stream, also after a failed block.
    Command(kStopTransmission, 0);
    WaitReady(kReadyTimeoutMs);
    Release();
    return ok;
}

bool SdSpiCard::WriteSectors(uint32_t sector, uint32_t count, const uint8_t* data)
{
    if (!IsReady() || data == nullptr)
    {
        m_LastError = "the card is not started";
        return false;
    }
    if (count == 0)
    {
        return true;
    }
    if (sector >= m_SectorCount || count > m_SectorCount - sector)
    {
        m_LastError = "write past the end of the card";
        return false;
    }

    m_Transport.Select();
    if (count == 1)
    {
        if (Command(kWriteSingleBlock, Address(sector)) != kR1Ready)
        {
            return Fail("the card refused a write");
        }
        const bool ok = WriteBlock(kStartBlockToken, data);
        Release();
        return ok;
    }

    if (Command(kWriteMultipleBlock, Address(sector)) != kR1Ready)
    {
        return Fail("the card refused a write");
    }
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i)
    {
        ok = WriteBlock(kStartMultiWriteToken, data + i * kSectorSize);
    }
    // The stop token ends the stream; the card is busy one byte later.
    Exchange(kStopMultiWriteToken);
    Exchange(0xFF);
    if (!WaitReady(kWriteTimeoutMs) && ok)
    {
        m_LastError = "timed out while the card was writing";
        ok = false;
    }
    Release();
    return ok;
}

bool SdSpiCard::Sync()
{
    if (!IsReady())
    {
        return false;
    }
    m_Transport.Select();
    const bool ok = WaitReady(kWriteTimeoutMs);
    Release();
    if (!ok)
    {
        m_LastError = "timed out while the card was writing";
    }
    return ok;
}

uint8_t SdSpiCard::Exchange(uint8_t value)
{
    uint8_t received = 0xFF;
    m_Transport.Transfer(&value, &received, 1);
    return received;
}

bool SdSpiCard::WaitReady(uint32_t timeoutMs)
{
    // A busy card holds its data line low.
    const uint32_t start = m_Transport.Milliseconds();
    do
    {
        if (Exchange(0xFF) == 0xFF)
        {
            return true;
        }
    } while (m_Transport.Milliseconds() - start < timeoutMs);
    return false;
}

uint8_t SdSpiCard::Command(uint8_t index, uint32_t argument)
{
    // A card still busy with a write would miss the command. CMD0 is sent to
    // a card in an unknown state, and CMD12 while data is still coming, so
    // neither waits.
    if (index != kGoIdleState && index != kStopTransmission && !WaitReady(kReadyTimeoutMs))
    {
        return kR1NoResponse;
    }

    uint8_t frame[6] = {
        static_cast<uint8_t>(0x40 | index),  static_cast<uint8_t>(argument >> 24), static_cast<uint8_t>(argument >> 16),
        static_cast<uint8_t>(argument >> 8), static_cast<uint8_t>(argument),       0,
    };
    frame[5] = CommandCrc(frame);
    m_Transport.Transfer(frame, nullptr, sizeof(frame));

    // The byte right after CMD12 is left over from the data and means nothing.
    if (index == kStopTransmission)
    {
        Exchange(0xFF);
    }

    // The response comes within eight bytes and has its top bit clear.
    for (int i = 0; i < kResponseBytes; ++i)
    {
        const uint8_t response = Exchange(0xFF);
        if ((response & 0x80) == 0)
        {
            return response;
        }
    }
    return kR1NoResponse;
}

uint8_t SdSpiCard::AppCommand(uint8_t index, uint32_t argument)
{
    const uint8_t response = Command(kAppCommandPrefix, 0);
    if (response > kR1Idle)
    {
        return response;
    }
    return Command(index, argument);
}

void SdSpiCard::Release()
{
    // One more byte after chip select goes high lets the card let go of its
    // data line, which matters when the bus is shared.
    m_Transport.Deselect();
    Exchange(0xFF);
}

bool SdSpiCard::ReadBlock(uint8_t* out, size_t length)
{
    const uint32_t start = m_Transport.Milliseconds();
    uint8_t token = 0xFF;
    do
    {
        token = Exchange(0xFF);
        if (token != 0xFF)
        {
            break;
        }
    } while (m_Transport.Milliseconds() - start < kReadTimeoutMs);

    if (token != kStartBlockToken)
    {
        m_LastError = token == 0xFF ? "timed out waiting for data" : "the card reported a read error";
        return false;
    }

    m_Transport.Transfer(nullptr, out, length);
    // The block's CRC16. CRC checks are off, so it is not checked.
    m_Transport.Transfer(nullptr, nullptr, 2);
    return true;
}

bool SdSpiCard::WriteBlock(uint8_t token, const uint8_t* data)
{
    // One byte of gap, the start token, the data and a dummy CRC16.
    Exchange(0xFF);
    Exchange(token);
    m_Transport.Transfer(data, nullptr, kSectorSize);
    m_Transport.Transfer(nullptr, nullptr, 2);

    const uint8_t response = Exchange(0xFF);
    if ((response & kDataResponseMask) != kDataAccepted)
    {
        m_LastError = "the card refused the data";
        return false;
    }
    if (!WaitReady(kWriteTimeoutMs))
    {
        m_LastError = "timed out while the card was writing";
        return false;
    }
    return true;
}

bool SdSpiCard::ReadCapacity()
{
    if (Command(kSendCsd, 0) != kR1Ready)
    {
        return Fail("the card did not send its CSD");
    }
    uint8_t csd[16] = {};
    if (!ReadBlock(csd, sizeof(csd)))
    {
        Release();
        return false;
    }

    // Byte 0 of the CSD holds bits 127-120; a field's bits are counted from
    // the end.
    const uint8_t structure = csd[0] >> 6;
    if (structure == 1)
    {
        // CSD version 2 (SDHC, SDXC): C_SIZE in bits 69-48 counts 512 KB units.
        const uint32_t size =
            (static_cast<uint32_t>(csd[7] & 0x3F) << 16) | (static_cast<uint32_t>(csd[8]) << 8) | csd[9];
        m_SectorCount = (size + 1) << 10;
        return true;
    }
    if (structure == 0)
    {
        // CSD version 1 (SDSC): (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of
        // 2^READ_BL_LEN bytes.
        const uint32_t blockLengthBits = csd[5] & 0x0F;
        const uint32_t size =
            (static_cast<uint32_t>(csd[6] & 0x03) << 10) | (static_cast<uint32_t>(csd[7]) << 2) | (csd[8] >> 6);
        const uint32_t multiplier = (static_cast<uint32_t>(csd[9] & 0x03) << 1) | (csd[10] >> 7);
        if (blockLengthBits < 9 || blockLengthBits > 11)
        {
            return Fail("the card's CSD gives an unknown block length");
        }
        m_SectorCount = (size + 1) << (multiplier + 2 + blockLengthBits - 9);
        return true;
    }
    return Fail("the card's CSD version is not supported");
}

uint32_t SdSpiCard::Address(uint32_t sector) const
{
    return m_Type == SdCardType::Sdhc ? sector : sector * kSectorSize;
}

bool SdSpiCard::Fail(const char* error)
{
    m_LastError = error;
    Release();
    return false;
}

}  // namespace DekiRaspberryPi
