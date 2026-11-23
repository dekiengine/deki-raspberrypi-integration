#include "PicoSDCard.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <cstring>

#if defined(DEKI_PICO)
#include <hardware/gpio.h>
#include <hardware/spi.h>
#include <pico/time.h>
#endif

namespace DekiRaspberryPi
{

namespace
{

// A GPIO's role in its SPI port, from its two low bits.
constexpr int kRoleMiso = 0;
constexpr int kRoleSck = 2;
constexpr int kRoleMosi = 3;

int SpiPortOf(int pin)
{
    return (pin >> 3) & 1;
}

int SpiRoleOf(int pin)
{
    return pin & 3;
}

}  // namespace

int PicoSpiPortForPins(int sck, int mosi, int miso)
{
    if (sck < 0 || mosi < 0 || miso < 0)
    {
        return -1;
    }
    if (SpiRoleOf(sck) != kRoleSck || SpiRoleOf(mosi) != kRoleMosi || SpiRoleOf(miso) != kRoleMiso)
    {
        return -1;
    }
    const int port = SpiPortOf(sck);
    return SpiPortOf(mosi) == port && SpiPortOf(miso) == port ? port : -1;
}

/// SdSpiCard's wires on a Pico SPI port, with chip select as a plain GPIO.
class PicoSpiTransport : public ISdSpiTransport
{
public:
    PicoSpiTransport(int port, int pinCs)
        : m_Port(port),
          m_PinCs(pinCs)
    {
    }

    /// Sets up the pins and the port. A port another driver has started
    /// already (a display on the same bus) is joined, not reset.
    void Begin(int pinSck, int pinMosi, int pinMiso)
    {
#if defined(DEKI_PICO)
        spi_inst_t* spi = Port();
        m_Shared = (spi_get_hw(spi)->cr1 & SPI_SSPCR1_SSE_BITS) != 0;
        if (!m_Shared)
        {
            spi_init(spi, m_ClockHz);
        }
        spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(static_cast<uint>(pinSck), GPIO_FUNC_SPI);
        gpio_set_function(static_cast<uint>(pinMosi), GPIO_FUNC_SPI);
        gpio_set_function(static_cast<uint>(pinMiso), GPIO_FUNC_SPI);
        // A card leaves its data line floating while it is not selected.
        gpio_pull_up(static_cast<uint>(pinMiso));

        gpio_init(static_cast<uint>(m_PinCs));
        gpio_put(static_cast<uint>(m_PinCs), true);
        gpio_set_dir(static_cast<uint>(m_PinCs), GPIO_OUT);
#else
        (void)pinSck;
        (void)pinMosi;
        (void)pinMiso;
#endif
    }

    void End()
    {
#if defined(DEKI_PICO)
        gpio_put(static_cast<uint>(m_PinCs), true);
        if (!m_Shared)
        {
            spi_deinit(Port());
        }
#endif
    }

    void Select() override
    {
#if defined(DEKI_PICO)
        // On a shared port the other driver may have changed the clock.
        if (m_Shared)
        {
            spi_set_baudrate(Port(), m_ClockHz);
            spi_set_format(Port(), 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        }
        gpio_put(static_cast<uint>(m_PinCs), false);
#endif
    }

    void Deselect() override
    {
#if defined(DEKI_PICO)
        gpio_put(static_cast<uint>(m_PinCs), true);
#endif
    }

    void Transfer(const uint8_t* tx, uint8_t* rx, size_t count) override
    {
#if defined(DEKI_PICO)
        spi_inst_t* spi = Port();
        if (tx != nullptr && rx != nullptr)
        {
            spi_write_read_blocking(spi, tx, rx, count);
        }
        else if (tx != nullptr)
        {
            spi_write_blocking(spi, tx, count);
        }
        else if (rx != nullptr)
        {
            spi_read_blocking(spi, 0xFF, rx, count);
        }
        else
        {
            uint8_t sink[16];
            while (count > 0)
            {
                const size_t step = std::min(count, sizeof(sink));
                spi_read_blocking(spi, 0xFF, sink, step);
                count -= step;
            }
        }
#else
        (void)tx;
        if (rx != nullptr)
        {
            std::memset(rx, 0xFF, count);
        }
#endif
    }

    void SetClock(uint32_t hz) override
    {
        m_ClockHz = hz;
#if defined(DEKI_PICO)
        spi_set_baudrate(Port(), hz);
#endif
    }

    uint32_t Milliseconds() override
    {
#if defined(DEKI_PICO)
        return to_ms_since_boot(get_absolute_time());
#else
        return ++m_FakeMilliseconds;
#endif
    }

private:
    int m_Port;
    int m_PinCs;
    uint32_t m_ClockHz = SdSpiCard::kInitClockHz;
    bool m_Shared = false;
#if defined(DEKI_PICO)
    spi_inst_t* Port() const { return m_Port == 1 ? spi1 : spi0; }
#else
    uint32_t m_FakeMilliseconds = 0;
#endif
};

PicoSDCard::PicoSDCard() = default;

PicoSDCard::~PicoSDCard()
{
    Shutdown();
}

void PicoSDCard::Configure(const Deki::PackageConfig& config)
{
    m_PinCLK = config.GetPin("CLK", -1);
    m_PinCD = config.GetPin("CD", -1);
    m_AutoMount = config.GetBool("auto_mount", true);
    m_MountPoint = config.GetString("mount_point", "/sdcard");

    const std::string mode = config.GetString("mode", "SPI");
    if (mode == "SDMMC4Bit")
    {
        m_Mode = DekiSdCard::SDCardMode::SDMMC4Bit;
    }
    else if (mode == "SDMMC1Bit")
    {
        m_Mode = DekiSdCard::SDCardMode::SDMMC1Bit;
    }
    else
    {
        m_Mode = DekiSdCard::SDCardMode::SPI;
        m_PinMOSI = config.GetPin("MOSI", -1);
        m_PinMISO = config.GetPin("MISO", -1);
        m_PinCS = config.GetPin("CS", -1);
        m_SpiFrequency = static_cast<uint32_t>(config.GetInt("spiHz", 20000000));
    }
}

bool PicoSDCard::Initialize()
{
    m_State = Deki::PackageState::Uninitialized;
    m_LastError.clear();

    if (m_Mode != DekiSdCard::SDCardMode::SPI)
    {
        return Fail("the Pico has no SD host, so only SPI mode works; set the card's mode to SPI");
    }
    if (m_PinCLK < 0 || m_PinMOSI < 0 || m_PinMISO < 0 || m_PinCS < 0)
    {
        return Fail("SPI mode needs the CLK, MOSI, MISO and CS pins");
    }
    const int port = PicoSpiPortForPins(m_PinCLK, m_PinMOSI, m_PinMISO);
    if (port < 0)
    {
        DEKI_LOG_ERROR("PicoSDCard: CLK=%d, MOSI=%d, MISO=%d are not the SCK, TX and RX pins of one SPI port", m_PinCLK,
                       m_PinMOSI, m_PinMISO);
        return Fail("CLK, MOSI and MISO must be the SCK, TX and RX pins of one SPI port");
    }

#if defined(DEKI_PICO)
    DEKI_LOG_INFO("PicoSDCard: SPI%d, CLK=%d, MOSI=%d, MISO=%d, CS=%d, CD=%d, %u kHz", port, m_PinCLK, m_PinMOSI,
                  m_PinMISO, m_PinCS, m_PinCD, static_cast<unsigned>(m_SpiFrequency / 1000));

    m_Transport = std::make_unique<PicoSpiTransport>(port, m_PinCS);
    m_Transport->Begin(m_PinCLK, m_PinMOSI, m_PinMISO);
    m_Card = std::make_unique<SdSpiCard>(*m_Transport);

    if (m_PinCD >= 0)
    {
        gpio_init(static_cast<uint>(m_PinCD));
        gpio_set_dir(static_cast<uint>(m_PinCD), GPIO_IN);
        gpio_pull_up(static_cast<uint>(m_PinCD));
    }
#else
    return Fail("the Pico SD card works only in a game built with the Pico SDK");
#endif

    m_FileSystem = std::make_unique<FatFileSystem>(m_Volume, m_MountPoint.c_str());
    m_Initialized = true;
    m_State = Deki::PackageState::Initialized;

    // The package still counts as initialised when this fails: the card may
    // not be in yet.
    if (m_AutoMount)
    {
        Mount();
    }
    return true;
}

void PicoSDCard::Shutdown()
{
    if (m_Initialized)
    {
        Unmount();
        m_FileSystem.reset();
        m_Card.reset();
        if (m_Transport)
        {
            m_Transport->End();
            m_Transport.reset();
        }
        m_Initialized = false;
    }
    m_State = Deki::PackageState::Disabled;
}

void PicoSDCard::Update(float deltaTime)
{
    (void)deltaTime;
    if (m_PinCD < 0 || !m_Initialized)
    {
        return;
    }
    const bool inserted = CheckCardDetect();
    if (inserted && m_CardState == DekiSdCard::SDCardState::NotMounted && m_AutoMount)
    {
        Mount();
    }
    else if (!inserted && m_CardState == DekiSdCard::SDCardState::Mounted)
    {
        Unmount();
    }
}

bool PicoSDCard::Mount()
{
    if (m_CardState == DekiSdCard::SDCardState::Mounted)
    {
        return true;
    }
    if (!m_Initialized || !m_Card)
    {
        m_LastError = "the SD card is not initialised";
        return false;
    }
    if (m_PinCD >= 0 && !CheckCardDetect())
    {
        m_LastError = "no card in the slot";
        m_CardState = DekiSdCard::SDCardState::NotMounted;
        return false;
    }

    m_CardState = DekiSdCard::SDCardState::Mounting;
    m_LastError.clear();
    if (!m_Card->Begin(m_SpiFrequency))
    {
        m_LastError = m_Card->LastError();
        DEKI_LOG_ERROR("PicoSDCard: %s", m_LastError.c_str());
        m_CardState = DekiSdCard::SDCardState::Error;
        return false;
    }
    if (!m_Volume.Mount(*m_Card))
    {
        m_Card->End();
        m_LastError = "the card holds no FAT32 or FAT16 volume";
        DEKI_LOG_ERROR("PicoSDCard: %s", m_LastError.c_str());
        m_CardState = DekiSdCard::SDCardState::Error;
        return false;
    }

    DEKI_LOG_INFO("PicoSDCard: mounted at %s, %u MB", m_MountPoint.c_str(),
                  static_cast<unsigned>(m_Volume.TotalBytes() / (1024 * 1024)));
    m_CardState = DekiSdCard::SDCardState::Mounted;
    return true;
}

void PicoSDCard::Unmount()
{
    if (m_CardState != DekiSdCard::SDCardState::Mounted)
    {
        return;
    }
    if (m_FileSystem)
    {
        m_FileSystem->CloseAll();
    }
    m_Volume.Unmount();
    if (m_Card)
    {
        m_Card->End();
    }
    m_CardState = DekiSdCard::SDCardState::NotMounted;
}

bool PicoSDCard::IsCardInserted() const
{
    if (m_PinCD >= 0)
    {
        return CheckCardDetect();
    }
    return m_CardState == DekiSdCard::SDCardState::Mounted;
}

uint64_t PicoSDCard::GetTotalBytes() const
{
    return m_CardState == DekiSdCard::SDCardState::Mounted ? m_Volume.TotalBytes() : 0;
}

uint64_t PicoSDCard::GetFreeBytes() const
{
    return m_CardState == DekiSdCard::SDCardState::Mounted ? m_Volume.FreeBytes() : 0;
}

Deki::IFileSystem* PicoSDCard::GetFileSystem()
{
    return m_CardState == DekiSdCard::SDCardState::Mounted ? m_FileSystem.get() : nullptr;
}

bool PicoSDCard::Fail(const char* error)
{
    m_LastError = error;
    DEKI_LOG_ERROR("PicoSDCard: %s", error);
    m_State = Deki::PackageState::Error;
    return false;
}

bool PicoSDCard::CheckCardDetect() const
{
#if defined(DEKI_PICO)
    // Card detect switches usually pull the pin low with a card in.
    return !gpio_get(static_cast<uint>(m_PinCD));
#else
    return true;
#endif
}

DekiSdCard::IDekiSDCard* CreatePicoSDCard()
{
    // The SD card component keeps the card for as long as the game runs.
    return new PicoSDCard();
}

}  // namespace DekiRaspberryPi
