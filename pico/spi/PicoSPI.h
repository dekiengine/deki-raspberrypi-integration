#pragma once

// deki-spi on the Pico SDK. Installing this package does not bring deki-spi:
// a project without it builds nothing here.
#if __has_include("IDekiSPI.h")

#include "IDekiSPI.h"  // from deki-spi

#include <deki/PackageConfig.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DekiRaspberryPi
{

class PicoSPIBus;

/// A device on a Pico SPI bus: its chip select and data/command pins, driven
/// as plain GPIOs, and its clock.
///
/// The SPI block sends single-line transfers only, so Dual and Quad
/// transfers are refused. A queued transfer goes out by DMA while the caller
/// prepares the next; queuing waits for the one before it, so at most one is
/// in flight and the gap between two is the few microseconds it takes to
/// switch the pins and start the DMA again. Queued transfers only write.
class PicoSPIDevice : public DekiSpi::IDekiSPIDevice
{
public:
    PicoSPIDevice(PicoSPIBus& bus, const DekiSpi::DeviceConfig& config);
    ~PicoSPIDevice() override;

    bool Transmit(const DekiSpi::Transfer& t) override;
    bool Queue(const DekiSpi::Transfer& t) override;
    bool Wait(size_t leaveInFlight, uint32_t timeoutMs) override;
    size_t InFlight() const override { return m_InFlight ? 1u : 0u; }
    bool Acquire() override;
    void Release() override;
    size_t GetMaxTransferBytes() const override;
    uint32_t GetClockHz() const override { return m_ClockHz; }

private:
    PicoSPIBus& m_Bus;
    DekiSpi::DeviceConfig m_Config;
    uint32_t m_ClockHz = 0;
    uint32_t m_ReadClockHz = 0;
    // The transfer the DMA is sending, and whether it keeps chip select low
    // after it.
    bool m_InFlight = false;
    bool m_InFlightKeepsCs = false;
    bool m_CsLow = false;
    int m_AcquireDepth = 0;
    // Copies of the last two short transfers queued (4 bytes or less), which
    // the caller may have on its stack.
    uint8_t m_ShortCopies[2][4] = {};
    int m_NextShortCopy = 0;

    // Waits for the DMA and the SPI block to finish the transfer in flight,
    // then lets chip select go unless that transfer keeps it.
    bool FinishInFlight(uint32_t timeoutMs);
    // Chip select low, the clock and format of this device, the DC level, and
    // the command and address phases: everything before a transfer's data.
    bool BeginTransfer(const DekiSpi::Transfer& t, uint32_t clockHz);
    void SelectChip(bool low);
};

/// An SPI bus on one of the Pico's two SPI blocks, with one DMA channel that
/// its devices share.
///
/// The block is the one the pins belong to: GPIO 0-7 and 16-23 are spi0's,
/// 8-15 and 24-29 spi1's (the pattern repeats every 16 pins on an RP2350B).
/// The spiHost setting is the number drivers find the bus by in DekiSPI.
class PicoSPIBus : public DekiSpi::IDekiSPIBus
{
public:
    PicoSPIBus() = default;
    ~PicoSPIBus() override;

    const char* GetPackageId() const override { return "spi"; }
    const char* GetPackageName() const override { return "SPI (Pico SDK)"; }
    void Configure(const Deki::PackageConfig& config) override;
    bool Initialize() override;
    void Shutdown() override;
    void Update(float) override {}
    Deki::PackageState GetState() const override { return m_State; }
    const char* GetLastError() const override { return m_LastError.c_str(); }

    int GetHost() const override { return m_Host; }
    int GetSclkPin() const override { return m_PinSclk; }
    int GetMosiPin() const override { return m_PinMosi; }
    int GetMisoPin() const override { return m_PinMiso; }
    bool IsQuad() const override { return false; }
    size_t GetMaxTransferBytes() const override { return m_MaxTransferBytes; }
    DekiSpi::IDekiSPIDevice* AddDevice(const DekiSpi::DeviceConfig& config) override;

    /// The SPI block GPIO `pin` belongs to (0 or 1), or -1 for no pin.
    static int BlockOfPin(int pin) { return pin < 0 ? -1 : ((pin & 15) >> 3) & 1; }

    // ---- for PicoSPIDevice --------------------------------------------------

    int Block() const { return m_Block; }
    int DmaChannel() const { return m_DmaChannel; }
    /// Sets the block's clock and mode for `device` unless it is already
    /// set so. Returns the clock it runs at.
    uint32_t UseFor(const PicoSPIDevice* device, uint32_t clockHz, uint8_t mode);

private:
    int m_Host = 0;
    int m_PinSclk = -1;
    int m_PinMosi = -1;
    int m_PinMiso = -1;
    int m_Block = -1;
    int m_DmaChannel = -1;
    size_t m_MaxTransferBytes = 65536;
    // Who the block's clock and mode were last set for.
    const PicoSPIDevice* m_CurrentDevice = nullptr;
    uint32_t m_CurrentClockHz = 0;
    std::vector<std::unique_ptr<PicoSPIDevice>> m_Devices;
    Deki::PackageState m_State = Deki::PackageState::Uninitialized;
    std::string m_LastError;

    bool Fail(const std::string& what);
};

}  // namespace DekiRaspberryPi

#endif  // __has_include("IDekiSPI.h")
