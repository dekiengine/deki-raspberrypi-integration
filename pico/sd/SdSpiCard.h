#pragma once

// An SD card in SPI mode. The SD protocol lives here; the bytes go through an
// ISdSpiTransport, so the same code drives a Pico SPI port on a board and a
// scripted fake card in the tests.

#include "BlockDevice.h"

#include <cstddef>
#include <cstdint>

namespace DekiRaspberryPi
{

/// The wires an SD card in SPI mode needs: chip select, a full-duplex byte
/// transfer, the clock rate, and a millisecond clock for timeouts.
class ISdSpiTransport
{
public:
    virtual ~ISdSpiTransport() = default;

    /// Drives chip select low.
    virtual void Select() = 0;

    /// Drives chip select high.
    virtual void Deselect() = 0;

    /// Sends `count` bytes and receives as many. A null `tx` sends 0xFF; a
    /// null `rx` drops what comes back.
    virtual void Transfer(const uint8_t* tx, uint8_t* rx, size_t count) = 0;

    /// Sets the SPI clock in Hz.
    virtual void SetClock(uint32_t hz) = 0;

    /// Milliseconds since some fixed point; only differences are used.
    virtual uint32_t Milliseconds() = 0;
};

/// What kind of card answered.
enum class SdCardType : uint8_t
{
    None,
    Sd1,   // SDSC, version 1: byte addresses
    Sd2,   // SDSC, version 2: byte addresses
    Sdhc,  // SDHC or SDXC: sector addresses
};

class SdSpiCard : public IBlockDevice
{
public:
    /// The clock used while the card starts up. The SD specification asks for
    /// 400 kHz at most.
    static constexpr uint32_t kInitClockHz = 400000;

    explicit SdSpiCard(ISdSpiTransport& transport);

    /// Wakes the card up and reads its size, then switches to `clockHz`.
    /// Returns false when no card answers or the card is not supported;
    /// LastError() says why.
    bool Begin(uint32_t clockHz);

    /// Forgets the card. Begin() starts it again.
    void End();

    bool IsReady() const { return m_Type != SdCardType::None; }
    SdCardType Type() const { return m_Type; }

    /// Why the last call failed, or "" when it did not.
    const char* LastError() const { return m_LastError; }

    // IBlockDevice
    bool ReadSectors(uint32_t sector, uint32_t count, uint8_t* out) override;
    bool WriteSectors(uint32_t sector, uint32_t count, const uint8_t* data) override;
    uint32_t SectorCount() const override { return m_SectorCount; }
    bool Sync() override;

    /// CRC7 of a command frame's first five bytes, shifted into place with
    /// the end bit set, as the sixth byte expects.
    static uint8_t CommandCrc(const uint8_t* frame);

private:
    ISdSpiTransport& m_Transport;
    SdCardType m_Type = SdCardType::None;
    uint32_t m_SectorCount = 0;
    const char* m_LastError = "";

    uint8_t Exchange(uint8_t value);
    bool WaitReady(uint32_t timeoutMs);
    uint8_t Command(uint8_t index, uint32_t argument);
    uint8_t AppCommand(uint8_t index, uint32_t argument);
    void Release();
    bool ReadBlock(uint8_t* out, size_t length);
    bool WriteBlock(uint8_t token, const uint8_t* data);
    bool ReadCapacity();
    uint32_t Address(uint32_t sector) const;
    bool Fail(const char* error);
};

}  // namespace DekiRaspberryPi
