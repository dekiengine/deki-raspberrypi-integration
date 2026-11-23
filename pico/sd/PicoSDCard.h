#pragma once

#include "FatFileSystem.h"
#include "FatVolume.h"
#include "SdSpiCard.h"

#include "IDekiSDCard.h"  // from deki-sdcard

#include <deki/PackageConfig.h>

#include <memory>
#include <string>

namespace DekiRaspberryPi
{

class PicoSpiTransport;

/// The SPI port (0 or 1) that has SCK, MOSI and MISO on these GPIOs, or -1
/// when the pins do not all belong to one port in those roles. On the RP2040
/// and RP2350 bit 3 of a GPIO number picks the port and its low two bits the
/// role: 0 is MISO (RX), 2 is SCK and 3 is MOSI (TX).
int PicoSpiPortForPins(int sck, int mosi, int miso);

/// DekiSdCard::IDekiSDCard for the Raspberry Pi Pico: an SD card in SPI mode
/// on one of the chip's SPI ports, with our own FAT32/FAT16 code on top.
///
/// Pins (from PackageConfig): CLK, MOSI, MISO, CS, and CD (card detect,
/// optional, active low). CLK picks the SPI port. CS can be any GPIO.
///
/// Settings:
/// - mode: only "SPI"; the Pico has no SD host, so SDMMC modes are refused
/// - auto_mount: "true" or "false"
/// - mount_point: what ConvertPath puts in front of paths (default "/sdcard")
/// - spiHz: the bus clock once the card is up (default 20 MHz)
class PicoSDCard : public DekiSdCard::IDekiSDCard
{
public:
    PicoSDCard();
    ~PicoSDCard() override;

    // Deki::IPackage
    const char* GetPackageId() const override { return "sd_card"; }
    const char* GetPackageName() const override { return "SD Card (Pico)"; }
    void Configure(const Deki::PackageConfig& config) override;
    bool Initialize() override;
    void Shutdown() override;
    void Update(float deltaTime) override;
    Deki::PackageState GetState() const override { return m_State; }
    const char* GetLastError() const override { return m_LastError.c_str(); }

    // DekiSdCard::IDekiSDCard
    bool Mount() override;
    void Unmount() override;
    DekiSdCard::SDCardState GetCardState() const override { return m_CardState; }
    bool IsCardInserted() const override;
    uint64_t GetTotalBytes() const override;
    uint64_t GetFreeBytes() const override;
    Deki::IFileSystem* GetFileSystem() override;
    const char* GetMountPoint() const override { return m_MountPoint.c_str(); }
    DekiSdCard::SDCardMode GetMode() const override { return m_Mode; }

private:
    int m_PinMOSI = -1;
    int m_PinMISO = -1;
    int m_PinCLK = -1;
    int m_PinCS = -1;
    int m_PinCD = -1;
    bool m_AutoMount = true;
    DekiSdCard::SDCardMode m_Mode = DekiSdCard::SDCardMode::SPI;
    uint32_t m_SpiFrequency = 20000000;
    std::string m_MountPoint = "/sdcard";

    Deki::PackageState m_State = Deki::PackageState::Uninitialized;
    DekiSdCard::SDCardState m_CardState = DekiSdCard::SDCardState::NotMounted;
    std::string m_LastError;
    bool m_Initialized = false;

    std::unique_ptr<PicoSpiTransport> m_Transport;
    std::unique_ptr<SdSpiCard> m_Card;
    // Counting free space reads the FAT once and keeps the count, which
    // GetFreeBytes (const in the interface) may do.
    mutable FatVolume m_Volume;
    std::unique_ptr<FatFileSystem> m_FileSystem;

    bool Fail(const char* error);
    bool CheckCardDetect() const;
};

/// Makes a PicoSDCard, for DekiSdCard::DekiSDCard::SetFactory.
DekiSdCard::IDekiSDCard* CreatePicoSDCard();

}  // namespace DekiRaspberryPi
