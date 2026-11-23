// The SD card protocol in SPI mode, against a fake card that answers byte by
// byte as a real one does: the start-up handshake for SDHC and both kinds of
// SDSC, single and multiple block reads and writes, and the timeouts. Then a
// FAT volume on the fake card, and the Pico SD card's pin and mode checks.
//
// A real card that gets a wrong byte answers nothing, so a bug here looks
// like "no card" on a board; the fake checks what a card would.

#include <gtest/gtest.h>

#include "pico/sd/FatVolume.h"
#include "pico/sd/PicoSDCard.h"
#include "pico/sd/SdSpiCard.h"

#include <deki/PackageConfig.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace DekiRaspberryPi;

namespace
{

enum class CardKind
{
    Sdhc,
    Sd2,  // SDSC that knows CMD8
    Sd1,  // SDSC from before CMD8
};

class FakeSdCard : public ISdSpiTransport
{
public:
    FakeSdCard(CardKind kind, uint32_t sectors)
        : image(static_cast<size_t>(sectors) * kSectorSize, 0),
          m_Kind(kind)
    {
        if (kind == CardKind::Sdhc)
        {
            // CSD version 2: C_SIZE counts 512 KB.
            const uint32_t size = sectors / 1024 - 1;
            m_Csd[0] = 0x40;
            m_Csd[7] = static_cast<uint8_t>((size >> 16) & 0x3F);
            m_Csd[8] = static_cast<uint8_t>(size >> 8);
            m_Csd[9] = static_cast<uint8_t>(size);
        }
        else
        {
            // CSD version 1 with 512-byte blocks and C_SIZE_MULT 7: (C_SIZE + 1) * 512 sectors.
            const uint32_t size = sectors / 512 - 1;
            m_Csd[5] = 0x09;
            m_Csd[6] = static_cast<uint8_t>((size >> 10) & 0x03);
            m_Csd[7] = static_cast<uint8_t>(size >> 2);
            m_Csd[8] = static_cast<uint8_t>((size & 0x03) << 6);
            m_Csd[9] = 0x03;
            m_Csd[10] = 0x80;
        }
    }

    void Select() override { m_Selected = true; }

    void Deselect() override
    {
        m_Selected = false;
        m_Out.clear();
        m_CommandLength = 0;
    }

    void Transfer(const uint8_t* tx, uint8_t* rx, size_t count) override
    {
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t out = Step(tx != nullptr ? tx[i] : 0xFF);
            if (rx != nullptr)
            {
                rx[i] = out;
            }
        }
    }

    void SetClock(uint32_t hz) override { clockHz = hz; }
    uint32_t Milliseconds() override { return ++m_Now; }

    std::vector<uint8_t> image;

    // What to get wrong.
    bool silent = false;
    bool neverReady = false;
    bool stallReads = false;
    bool busyForever = false;

    // What was seen.
    uint32_t clockHz = 0;
    uint32_t clocksBeforeGoIdle = 0;
    uint32_t goIdleClockHz = 0;
    bool badAddress = false;
    std::vector<uint8_t> commands;

private:
    enum class State
    {
        Command,
        WriteToken,
        WriteData,
    };

    CardKind m_Kind;
    uint8_t m_Csd[16] = {};
    uint32_t m_Now = 0;
    bool m_Selected = false;
    bool m_SawGoIdle = false;
    bool m_AppNext = false;
    int m_OpCondCalls = 0;
    bool m_Busy = false;
    std::deque<uint8_t> m_Out;
    uint8_t m_Command[6] = {};
    int m_CommandLength = 0;
    State m_State = State::Command;
    bool m_MultiWrite = false;
    bool m_MultiRead = false;
    uint32_t m_Sector = 0;
    std::vector<uint8_t> m_Data;

    uint8_t Step(uint8_t in)
    {
        if (!m_Selected)
        {
            if (!m_SawGoIdle)
            {
                clocksBeforeGoIdle += 8;
            }
            return 0xFF;
        }
        if (silent)
        {
            return 0xFF;
        }

        uint8_t out = 0xFF;
        if (!m_Out.empty())
        {
            out = m_Out.front();
            m_Out.pop_front();
        }
        else if (m_Busy)
        {
            out = 0x00;
        }
        else if (m_MultiRead && m_Sector < image.size() / kSectorSize)
        {
            QueueBlock(m_Sector++);
        }
        Take(in);
        return out;
    }

    void Take(uint8_t in)
    {
        switch (m_State)
        {
            case State::Command:
                if (m_CommandLength == 0 && (in & 0xC0) != 0x40)
                {
                    return;
                }
                m_Command[m_CommandLength++] = in;
                if (m_CommandLength == 6)
                {
                    m_CommandLength = 0;
                    Execute();
                }
                return;
            case State::WriteToken:
                if (in == (m_MultiWrite ? 0xFC : 0xFE))
                {
                    m_State = State::WriteData;
                    m_Data.clear();
                }
                else if (m_MultiWrite && in == 0xFD)
                {
                    commands.push_back(0xFD);
                    m_Out.insert(m_Out.end(), { 0xFF, 0x00, 0x00, 0x00 });
                    m_State = State::Command;
                }
                return;
            case State::WriteData:
                m_Data.push_back(in);
                if (m_Data.size() == kSectorSize + 2)
                {
                    std::memcpy(image.data() + static_cast<size_t>(m_Sector) * kSectorSize, m_Data.data(), kSectorSize);
                    ++m_Sector;
                    // The data response (bits xxx00101), then busy.
                    m_Out.push_back(0xE5);
                    if (busyForever)
                    {
                        m_Busy = true;
                    }
                    else
                    {
                        m_Out.insert(m_Out.end(), { 0x00, 0x00, 0x00, 0x00 });
                    }
                    m_State = m_MultiWrite ? State::WriteToken : State::Command;
                }
                return;
        }
    }

    void Respond(std::initializer_list<uint8_t> bytes)
    {
        m_Out.push_back(0xFF);  // a real card answers a byte or more later
        m_Out.insert(m_Out.end(), bytes);
    }

    void QueueBlock(uint32_t sector)
    {
        m_Out.insert(m_Out.end(), { 0xFF, 0xFE });
        const uint8_t* data = image.data() + static_cast<size_t>(sector) * kSectorSize;
        m_Out.insert(m_Out.end(), data, data + kSectorSize);
        m_Out.insert(m_Out.end(), { 0x12, 0x34 });
    }

    bool SectorFor(uint32_t argument, uint32_t& sector)
    {
        if (m_Kind == CardKind::Sdhc)
        {
            sector = argument;
        }
        else
        {
            if (argument % kSectorSize != 0)
            {
                badAddress = true;
                return false;
            }
            sector = argument / kSectorSize;
        }
        return sector < image.size() / kSectorSize;
    }

    void Execute()
    {
        const uint8_t index = m_Command[0] & 0x3F;
        const uint32_t argument = (static_cast<uint32_t>(m_Command[1]) << 24) |
                                  (static_cast<uint32_t>(m_Command[2]) << 16) |
                                  (static_cast<uint32_t>(m_Command[3]) << 8) | m_Command[4];
        commands.push_back(index);

        // CRC is checked for CMD0 and CMD8 even in SPI mode. These are the
        // values the SD specification prints.
        if ((index == 0 && m_Command[5] != 0x95) || (index == 8 && argument == 0x1AA && m_Command[5] != 0x87))
        {
            Respond({ 0x09 });
            return;
        }
        const bool app = m_AppNext;
        m_AppNext = false;
        if (!m_SawGoIdle && index != 0)
        {
            return;
        }

        const uint8_t r1 = m_OpCondCalls >= 3 ? 0x00 : 0x01;
        uint32_t sector = 0;
        switch (index)
        {
            case 0:
                if (!m_SawGoIdle)
                {
                    goIdleClockHz = clockHz;
                }
                m_SawGoIdle = true;
                m_OpCondCalls = 0;
                Respond({ 0x01 });
                return;
            case 8:
                if (m_Kind == CardKind::Sd1)
                {
                    Respond({ 0x05 });
                }
                else
                {
                    Respond({ 0x01, 0x00, 0x00, 0x01, static_cast<uint8_t>(argument) });
                }
                return;
            case 55:
                m_AppNext = true;
                Respond({ r1 });
                return;
            case 41:
                if (!app)
                {
                    Respond({ 0x05 });
                    return;
                }
                // An SDHC card stays idle until the host says it can address it.
                if (!neverReady && (m_Kind != CardKind::Sdhc || (argument & 0x40000000) != 0))
                {
                    ++m_OpCondCalls;
                }
                Respond({ m_OpCondCalls >= 3 ? uint8_t{ 0x00 } : uint8_t{ 0x01 } });
                return;
            case 58:
                Respond({ r1, static_cast<uint8_t>(m_Kind == CardKind::Sdhc ? 0xC0 : 0x80), 0xFF, 0x80, 0x00 });
                return;
            case 59: Respond({ r1 }); return;
            case 16: Respond({ static_cast<uint8_t>(argument == kSectorSize ? 0x00 : 0x40) }); return;
            case 9:
                Respond({ 0x00, 0xFF, 0xFE });
                m_Out.insert(m_Out.end(), m_Csd, m_Csd + 16);
                m_Out.insert(m_Out.end(), { 0xAB, 0xCD });
                return;
            case 17:
                if (!SectorFor(argument, sector))
                {
                    Respond({ 0x20 });
                    return;
                }
                Respond({ 0x00, 0xFF });
                if (!stallReads)
                {
                    QueueBlock(sector);
                }
                return;
            case 18:
                if (!SectorFor(argument, sector))
                {
                    Respond({ 0x20 });
                    return;
                }
                Respond({ 0x00 });
                m_MultiRead = true;
                m_Sector = sector;
                return;
            case 12:
                // The byte after CMD12 is junk, then R1, then busy.
                m_Out.clear();
                m_MultiRead = false;
                m_Out.insert(m_Out.end(), { 0x3C, 0x00, 0x00, 0x00 });
                return;
            case 24:
            case 25:
                if (!SectorFor(argument, sector))
                {
                    Respond({ 0x20 });
                    return;
                }
                Respond({ 0x00 });
                m_Sector = sector;
                m_MultiWrite = index == 25;
                m_State = State::WriteToken;
                return;
            case 13: Respond({ 0x00, 0x00 }); return;
            default: Respond({ 0x04 }); return;
        }
    }
};

std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
{
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8_t>(seed + i * 13 + (i >> 9));
    }
    return bytes;
}

bool Saw(const FakeSdCard& card, uint8_t command)
{
    return std::find(card.commands.begin(), card.commands.end(), command) != card.commands.end();
}

}  // namespace

TEST(SdSpiCard, CommandCrcMatchesTheSpecification)
{
    const uint8_t goIdle[5] = { 0x40, 0, 0, 0, 0 };
    const uint8_t ifCond[5] = { 0x48, 0, 0, 0x01, 0xAA };
    EXPECT_EQ(SdSpiCard::CommandCrc(goIdle), 0x95);
    EXPECT_EQ(SdSpiCard::CommandCrc(ifCond), 0x87);
}

TEST(SdSpiCard, StartsAnSdhcCardWithSectorAddresses)
{
    FakeSdCard fake(CardKind::Sdhc, 4096);
    SdSpiCard card(fake);
    ASSERT_TRUE(card.Begin(20000000)) << card.LastError();

    EXPECT_EQ(card.Type(), SdCardType::Sdhc);
    EXPECT_EQ(card.SectorCount(), 4096u);
    EXPECT_GE(fake.clocksBeforeGoIdle, 74u) << "power-up clocks with chip select high";
    EXPECT_LE(fake.goIdleClockHz, 400000u);
    EXPECT_EQ(fake.clockHz, 20000000u) << "fast once started";
    EXPECT_TRUE(Saw(fake, 58)) << "CCS read from the OCR";
    EXPECT_FALSE(Saw(fake, 16)) << "an SDHC card has fixed 512-byte blocks";

    std::vector<uint8_t> block = Pattern(kSectorSize, 1);
    ASSERT_TRUE(card.WriteSectors(4095, 1, block.data())) << card.LastError();
    EXPECT_EQ(std::memcmp(fake.image.data() + 4095 * kSectorSize, block.data(), kSectorSize), 0);
    std::vector<uint8_t> back(kSectorSize);
    ASSERT_TRUE(card.ReadSectors(4095, 1, back.data())) << card.LastError();
    EXPECT_EQ(back, block);
    EXPECT_FALSE(card.ReadSectors(4096, 1, back.data())) << "past the end";
}

TEST(SdSpiCard, StartsAVersion2SdscCardWithByteAddresses)
{
    FakeSdCard fake(CardKind::Sd2, 2048);
    SdSpiCard card(fake);
    ASSERT_TRUE(card.Begin(10000000)) << card.LastError();
    EXPECT_EQ(card.Type(), SdCardType::Sd2);
    EXPECT_EQ(card.SectorCount(), 2048u);
    EXPECT_TRUE(Saw(fake, 16)) << "block length set to 512";

    std::vector<uint8_t> block = Pattern(kSectorSize, 2);
    ASSERT_TRUE(card.WriteSectors(1000, 1, block.data())) << card.LastError();
    std::vector<uint8_t> back(kSectorSize);
    ASSERT_TRUE(card.ReadSectors(1000, 1, back.data())) << card.LastError();
    EXPECT_EQ(back, block);
    EXPECT_EQ(std::memcmp(fake.image.data() + 1000 * kSectorSize, block.data(), kSectorSize), 0);
    EXPECT_FALSE(fake.badAddress);
}

TEST(SdSpiCard, StartsAVersion1CardThatDoesNotKnowCmd8)
{
    FakeSdCard fake(CardKind::Sd1, 1024);
    SdSpiCard card(fake);
    ASSERT_TRUE(card.Begin(10000000)) << card.LastError();
    EXPECT_EQ(card.Type(), SdCardType::Sd1);
    EXPECT_EQ(card.SectorCount(), 1024u);
    EXPECT_FALSE(Saw(fake, 58));

    std::vector<uint8_t> block = Pattern(kSectorSize, 3);
    ASSERT_TRUE(card.WriteSectors(3, 1, block.data()));
    std::vector<uint8_t> back(kSectorSize);
    ASSERT_TRUE(card.ReadSectors(3, 1, back.data()));
    EXPECT_EQ(back, block);
    EXPECT_FALSE(fake.badAddress);
}

TEST(SdSpiCard, ReadsAndWritesSeveralBlocksInOneCommand)
{
    FakeSdCard fake(CardKind::Sdhc, 2048);
    SdSpiCard card(fake);
    ASSERT_TRUE(card.Begin(20000000));

    const std::vector<uint8_t> data = Pattern(7 * kSectorSize, 4);
    ASSERT_TRUE(card.WriteSectors(100, 7, data.data())) << card.LastError();
    EXPECT_TRUE(Saw(fake, 25));
    EXPECT_TRUE(Saw(fake, 0xFD)) << "the stop token";
    EXPECT_EQ(std::memcmp(fake.image.data() + 100 * kSectorSize, data.data(), data.size()), 0);

    std::vector<uint8_t> back(data.size());
    ASSERT_TRUE(card.ReadSectors(100, 7, back.data())) << card.LastError();
    EXPECT_TRUE(Saw(fake, 18));
    EXPECT_TRUE(Saw(fake, 12));
    EXPECT_EQ(back, data);

    // The card takes commands again after CMD12.
    ASSERT_TRUE(card.ReadSectors(102, 1, back.data())) << card.LastError();
    EXPECT_EQ(std::memcmp(back.data(), data.data() + 2 * kSectorSize, kSectorSize), 0);
    EXPECT_TRUE(card.Sync());
}

TEST(SdSpiCard, NoCardMeansNoAnswer)
{
    FakeSdCard fake(CardKind::Sdhc, 1024);
    fake.silent = true;
    SdSpiCard card(fake);
    EXPECT_FALSE(card.Begin(20000000));
    EXPECT_STREQ(card.LastError(), "no card answered");
    EXPECT_FALSE(card.IsReady());
    std::vector<uint8_t> block(kSectorSize);
    EXPECT_FALSE(card.ReadSectors(0, 1, block.data()));
}

TEST(SdSpiCard, ACardThatNeverFinishesStartingTimesOut)
{
    FakeSdCard fake(CardKind::Sdhc, 1024);
    fake.neverReady = true;
    SdSpiCard card(fake);
    EXPECT_FALSE(card.Begin(20000000));
    EXPECT_STREQ(card.LastError(), "timed out waiting for the card to start");
}

TEST(SdSpiCard, ReadsAndWritesThatNeverFinishTimeOut)
{
    FakeSdCard fake(CardKind::Sdhc, 1024);
    SdSpiCard card(fake);
    ASSERT_TRUE(card.Begin(20000000));

    std::vector<uint8_t> block(kSectorSize);
    fake.stallReads = true;
    EXPECT_FALSE(card.ReadSectors(1, 1, block.data()));
    EXPECT_STREQ(card.LastError(), "timed out waiting for data");
    fake.stallReads = false;

    fake.busyForever = true;
    EXPECT_FALSE(card.WriteSectors(1, 1, block.data()));
    EXPECT_STREQ(card.LastError(), "timed out while the card was writing");
}

TEST(SdSpiCard, CarriesAFatVolume)
{
    FakeSdCard fake(CardKind::Sdhc, 32768);
    SdSpiCard card(fake);
    ASSERT_TRUE(card.Begin(20000000));

    FatFormatOptions options;
    options.type = FatType::Fat16;
    options.sectorsPerCluster = 4;  // whole clusters go as multi-block commands
    ASSERT_TRUE(FatVolume::Format(card, options));

    FatVolume volume;
    ASSERT_TRUE(volume.Mount(card));
    ASSERT_TRUE(volume.CreateDirectory("save"));
    const std::vector<uint8_t> data = Pattern(50000, 5);
    FatFile file;
    ASSERT_TRUE(volume.Open(file, "save/Slot One.bin", FatOpenMode::Write));
    ASSERT_EQ(volume.Write(file, data.data(), data.size()), data.size());
    ASSERT_TRUE(volume.Close(file));
    volume.Unmount();

    ASSERT_TRUE(volume.Mount(card));
    ASSERT_TRUE(volume.Open(file, "save/slot one.bin", FatOpenMode::Read));
    std::vector<uint8_t> back(file.size);
    EXPECT_EQ(volume.Read(file, back.data(), back.size()), data.size());
    volume.Close(file);
    EXPECT_EQ(back, data);
    EXPECT_TRUE(Saw(fake, 25));
    EXPECT_TRUE(Saw(fake, 18));
}

TEST(PicoSDCard, PicksTheSpiPortFromThePins)
{
    EXPECT_EQ(PicoSpiPortForPins(2, 3, 4), 0);
    EXPECT_EQ(PicoSpiPortForPins(18, 19, 16), 0);
    EXPECT_EQ(PicoSpiPortForPins(10, 11, 12), 1);
    EXPECT_EQ(PicoSpiPortForPins(14, 15, 8), 1);
    EXPECT_EQ(PicoSpiPortForPins(26, 27, 28), 1);
    EXPECT_EQ(PicoSpiPortForPins(2, 11, 4), -1) << "MOSI on the other port";
    EXPECT_EQ(PicoSpiPortForPins(3, 2, 4), -1) << "SCK and MOSI swapped";
    EXPECT_EQ(PicoSpiPortForPins(2, 3, 5), -1) << "a CS pin is not MISO";
    EXPECT_EQ(PicoSpiPortForPins(-1, 3, 4), -1);
}

TEST(PicoSDCard, RefusesSdmmcModesAndPinsThatAreNotOnePort)
{
    Deki::PackageConfig config;
    config.settings["mode"] = "SDMMC4Bit";
    config.pins["CLK"] = 2;
    PicoSDCard sdmmc;
    sdmmc.Configure(config);
    EXPECT_FALSE(sdmmc.Initialize());
    EXPECT_NE(std::string(sdmmc.GetLastError()).find("only SPI mode"), std::string::npos);
    EXPECT_EQ(sdmmc.GetState(), Deki::PackageState::Error);

    config.settings["mode"] = "SPI";
    config.pins["CLK"] = 2;
    config.pins["MOSI"] = 11;
    config.pins["MISO"] = 4;
    config.pins["CS"] = 5;
    PicoSDCard mixed;
    mixed.Configure(config);
    EXPECT_FALSE(mixed.Initialize());
    EXPECT_NE(std::string(mixed.GetLastError()).find("one SPI port"), std::string::npos);

    config.pins["MOSI"] = 3;
    PicoSDCard good;
    good.Configure(config);
    EXPECT_FALSE(good.Initialize()) << "not a Pico build";
    EXPECT_NE(std::string(good.GetLastError()).find("Pico SDK"), std::string::npos);
    EXPECT_EQ(good.GetFileSystem(), nullptr);

    DekiSdCard::IDekiSDCard* made = CreatePicoSDCard();
    ASSERT_NE(made, nullptr);
    EXPECT_STREQ(made->GetPackageId(), "sd_card");
    EXPECT_EQ(made->GetMode(), DekiSdCard::SDCardMode::SPI);
    delete made;
}
