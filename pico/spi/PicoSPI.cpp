#include "PicoSPI.h"

#if __has_include("IDekiSPI.h")

#include <deki/LogSystem.h>
#include <deki/Time.h>

#include <cstring>

#if defined(DEKI_PICO)
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/spi.h>
#endif

namespace DekiRaspberryPi
{

namespace
{

#if defined(DEKI_PICO)
spi_inst_t* SpiBlock(int block)
{
    return block == 1 ? spi1 : spi0;
}

// Sends `bytes` and waits until they are out. Bytes clocked in meanwhile are
// dropped.
void WriteBlocking(int block, const uint8_t* bytes, size_t count)
{
    spi_write_blocking(SpiBlock(block), bytes, count);
}
#endif

// The command or address phase as bytes, most significant first. Phases are
// whole bytes on this block.
size_t PhaseBytes(uint32_t value, uint8_t bits, uint8_t* out)
{
    const size_t count = bits / 8;
    for (size_t i = 0; i < count; ++i)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * (count - 1 - i)));
    }
    return count;
}

}  // namespace

// ============================================================================
// PicoSPIDevice
// ============================================================================

PicoSPIDevice::PicoSPIDevice(PicoSPIBus& bus, const DekiSpi::DeviceConfig& config)
    : m_Bus(bus),
      m_Config(config),
      m_ClockHz(config.clockHz),
      m_ReadClockHz(config.readClockHz > 0 ? config.readClockHz : config.clockHz)
{
#if defined(DEKI_PICO)
    if (m_Config.csPin >= 0)
    {
        gpio_init(static_cast<uint>(m_Config.csPin));
        gpio_put(static_cast<uint>(m_Config.csPin), true);
        gpio_set_dir(static_cast<uint>(m_Config.csPin), GPIO_OUT);
    }
    if (m_Config.dcPin >= 0)
    {
        gpio_init(static_cast<uint>(m_Config.dcPin));
        gpio_put(static_cast<uint>(m_Config.dcPin), true);
        gpio_set_dir(static_cast<uint>(m_Config.dcPin), GPIO_OUT);
    }
    // What the block actually makes of the clock asked for.
    m_ClockHz = m_Bus.UseFor(this, m_ClockHz, m_Config.mode);
#endif
}

PicoSPIDevice::~PicoSPIDevice()
{
    FinishInFlight(1000);
    SelectChip(false);
}

void PicoSPIDevice::SelectChip(bool low)
{
    if (m_CsLow == low)
    {
        return;
    }
    m_CsLow = low;
#if defined(DEKI_PICO)
    if (m_Config.csPin >= 0)
    {
        gpio_put(static_cast<uint>(m_Config.csPin), !low);
    }
#endif
}

size_t PicoSPIDevice::GetMaxTransferBytes() const
{
    return m_Bus.GetMaxTransferBytes();
}

bool PicoSPIDevice::FinishInFlight(uint32_t timeoutMs)
{
    if (!m_InFlight)
    {
        return true;
    }
#if defined(DEKI_PICO)
    const uint32_t start = Deki::Time::GetTime();
    while (dma_channel_is_busy(static_cast<uint>(m_Bus.DmaChannel())))
    {
        if (timeoutMs != UINT32_MAX && Deki::Time::GetTime() - start > timeoutMs)
        {
            DEKI_LOG_ERROR("PicoSPI: a transfer did not finish in %u ms", static_cast<unsigned>(timeoutMs));
            dma_channel_abort(static_cast<uint>(m_Bus.DmaChannel()));
            break;
        }
    }
    // The DMA is done once the last byte is in the FIFO; the block still
    // has to shift it out before chip select may go.
    spi_inst_t* spi = SpiBlock(m_Bus.Block());
    while (spi_is_busy(spi))
    {
    }
    // A write fills the receive FIFO with what came back; empty it and clear
    // the overrun it raised.
    while (spi_is_readable(spi))
    {
        (void)spi_get_hw(spi)->dr;
    }
    spi_get_hw(spi)->icr = SPI_SSPICR_RORIC_BITS;
#else
    (void)timeoutMs;
#endif
    m_InFlight = false;
    if (!m_InFlightKeepsCs)
    {
        SelectChip(false);
    }
    return true;
}

bool PicoSPIDevice::BeginTransfer(const DekiSpi::Transfer& t, uint32_t clockHz)
{
    if (t.lines != DekiSpi::Lines::Single)
    {
        DEKI_LOG_ERROR("PicoSPI: the Pico's SPI block has one data line; a %d-line transfer was refused",
                       static_cast<int>(t.lines));
        return false;
    }
    if (t.cmdBits % 8 != 0 || t.addrBits % 8 != 0 || t.addrBits > 32)
    {
        DEKI_LOG_ERROR("PicoSPI: command and address phases must be whole bytes");
        return false;
    }
    m_Bus.UseFor(this, clockHz, m_Config.mode);
#if defined(DEKI_PICO)
    if (t.dc >= 0 && m_Config.dcPin >= 0)
    {
        gpio_put(static_cast<uint>(m_Config.dcPin), t.dc != 0);
    }
#endif
    SelectChip(true);
    uint8_t phases[2 + 4];
    size_t count = PhaseBytes(t.cmd, t.cmdBits, phases);
    count += PhaseBytes(t.addr, t.addrBits, phases + count);
#if defined(DEKI_PICO)
    if (count > 0)
    {
        WriteBlocking(m_Bus.Block(), phases, count);
    }
#endif
    return true;
}

bool PicoSPIDevice::Transmit(const DekiSpi::Transfer& t)
{
    if (!FinishInFlight(UINT32_MAX))
    {
        return false;
    }
    const bool reads = t.rx && t.rxBytes > 0;
    if (m_Config.threeWire && reads)
    {
        DEKI_LOG_ERROR("PicoSPI: three-wire reads are not supported");
        return false;
    }
    if (!BeginTransfer(t, m_ClockHz))
    {
        return false;
    }
#if defined(DEKI_PICO)
    spi_inst_t* spi = SpiBlock(m_Bus.Block());
    const auto* tx = static_cast<const uint8_t*>(t.tx);
    auto* rx = static_cast<uint8_t*>(t.rx);
    if (reads && !m_Config.halfDuplex)
    {
        // Full duplex: the reply comes in while the data goes out.
        const size_t both = t.rxBytes < t.txBytes ? t.rxBytes : t.txBytes;
        spi_write_read_blocking(spi, tx, rx, both);
        if (t.txBytes > both)
        {
            spi_write_blocking(spi, tx + both, t.txBytes - both);
        }
    }
    else
    {
        if (tx && t.txBytes > 0)
        {
            spi_write_blocking(spi, tx, t.txBytes);
        }
        if (reads)
        {
            m_Bus.UseFor(this, m_ReadClockHz, m_Config.mode);
            spi_read_blocking(spi, 0xFF, rx, t.rxBytes);
        }
    }
#endif
    if (!t.keepCs || m_AcquireDepth == 0)
    {
        SelectChip(false);
    }
    return true;
}

bool PicoSPIDevice::Queue(const DekiSpi::Transfer& t)
{
    if (t.rx && t.rxBytes > 0)
    {
        DEKI_LOG_ERROR("PicoSPI: queued transfers only write; read with Transmit");
        return false;
    }
    if (t.txBytes > GetMaxTransferBytes())
    {
        return false;
    }
    // A short transfer may live on the caller's stack: send a copy.
    const uint8_t* tx = static_cast<const uint8_t*>(t.tx);
    if (tx && t.txBytes <= 4)
    {
        uint8_t* copy = m_ShortCopies[m_NextShortCopy];
        m_NextShortCopy ^= 1;
        std::memcpy(copy, tx, t.txBytes);
        tx = copy;
    }
    if (!FinishInFlight(UINT32_MAX) || !BeginTransfer(t, m_ClockHz))
    {
        return false;
    }
    m_InFlightKeepsCs = t.keepCs && m_AcquireDepth > 0;
    if (!tx || t.txBytes == 0)
    {
        // Phases only: they were sent by BeginTransfer.
        m_InFlight = true;
        return FinishInFlight(UINT32_MAX);
    }
#if defined(DEKI_PICO)
    const uint channel = static_cast<uint>(m_Bus.DmaChannel());
    spi_inst_t* spi = SpiBlock(m_Bus.Block());
    dma_channel_config cfg = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(spi, true));
    dma_channel_configure(channel, &cfg, &spi_get_hw(spi)->dr, tx, t.txBytes, true);
#endif
    m_InFlight = true;
    return true;
}

bool PicoSPIDevice::Wait(size_t leaveInFlight, uint32_t timeoutMs)
{
    // At most one transfer is ever in flight.
    if (leaveInFlight >= 1)
    {
        return true;
    }
    return FinishInFlight(timeoutMs);
}

bool PicoSPIDevice::Acquire()
{
    ++m_AcquireDepth;
    return true;
}

void PicoSPIDevice::Release()
{
    if (m_AcquireDepth == 0)
    {
        return;
    }
    if (--m_AcquireDepth == 0)
    {
        FinishInFlight(UINT32_MAX);
        SelectChip(false);
    }
}

// ============================================================================
// PicoSPIBus
// ============================================================================

PicoSPIBus::~PicoSPIBus()
{
    Shutdown();
}

void PicoSPIBus::Configure(const Deki::PackageConfig& config)
{
    m_PinSclk = config.GetPin("SCLK", -1);
    m_PinMosi = config.GetPin("MOSI", -1);
    m_PinMiso = config.GetPin("MISO", -1);
    m_Host = config.GetInt("spiHost", 0);
    const int maxBytes = config.GetInt("maxTransferBytes", 65536);
    m_MaxTransferBytes = maxBytes > 0 ? static_cast<size_t>(maxBytes) : 65536;
    if (config.GetPin("IO2", -1) >= 0 || config.GetPin("IO3", -1) >= 0)
    {
        DEKI_LOG_WARNING("PicoSPI: IO2 and IO3 are ignored: the Pico's SPI block has one data line");
    }
}

bool PicoSPIBus::Fail(const std::string& what)
{
    m_LastError = what;
    m_State = Deki::PackageState::Error;
    return false;
}

bool PicoSPIBus::Initialize()
{
    if (m_State == Deki::PackageState::Initialized)
    {
        return true;
    }
    if (m_PinSclk < 0)
    {
        return Fail("PicoSPI: no clock pin configured");
    }
    m_Block = BlockOfPin(m_PinSclk);
    for (int pin : { m_PinMosi, m_PinMiso })
    {
        if (pin >= 0 && BlockOfPin(pin) != m_Block)
        {
            return Fail("PicoSPI: GPIO " + std::to_string(pin) + " is not on spi" + std::to_string(m_Block) +
                        " like the clock pin " + std::to_string(m_PinSclk));
        }
    }
#if defined(DEKI_PICO)
    spi_init(SpiBlock(m_Block), 1000000);
    for (int pin : { m_PinSclk, m_PinMosi, m_PinMiso })
    {
        if (pin >= 0)
        {
            gpio_set_function(static_cast<uint>(pin), GPIO_FUNC_SPI);
        }
    }
    m_DmaChannel = dma_claim_unused_channel(false);
    if (m_DmaChannel < 0)
    {
        spi_deinit(SpiBlock(m_Block));
        return Fail("PicoSPI: no free DMA channel");
    }
    DEKI_LOG_INFO("PicoSPI: spi%d on SCK %d, MOSI %d, MISO %d", m_Block, m_PinSclk, m_PinMosi, m_PinMiso);
#endif
    m_State = Deki::PackageState::Initialized;
    return true;
}

void PicoSPIBus::Shutdown()
{
    m_Devices.clear();
#if defined(DEKI_PICO)
    if (m_State == Deki::PackageState::Initialized)
    {
        if (m_DmaChannel >= 0)
        {
            dma_channel_unclaim(static_cast<uint>(m_DmaChannel));
        }
        spi_deinit(SpiBlock(m_Block));
    }
#endif
    m_DmaChannel = -1;
    m_CurrentDevice = nullptr;
    m_State = Deki::PackageState::Uninitialized;
}

DekiSpi::IDekiSPIDevice* PicoSPIBus::AddDevice(const DekiSpi::DeviceConfig& config)
{
    if (m_State != Deki::PackageState::Initialized)
    {
        return nullptr;
    }
    if (config.threeWire)
    {
        DEKI_LOG_ERROR("PicoSPI: three-wire devices are not supported");
        return nullptr;
    }
    m_Devices.push_back(std::make_unique<PicoSPIDevice>(*this, config));
    return m_Devices.back().get();
}

uint32_t PicoSPIBus::UseFor(const PicoSPIDevice* device, uint32_t clockHz, uint8_t mode)
{
    if (device == m_CurrentDevice && clockHz == m_CurrentClockHz)
    {
        return m_CurrentClockHz;
    }
    m_CurrentDevice = device;
    m_CurrentClockHz = clockHz;
#if defined(DEKI_PICO)
    spi_inst_t* spi = SpiBlock(m_Block);
    const uint32_t actual = spi_set_baudrate(spi, clockHz);
    const spi_cpol_t cpol = (mode & 2) ? SPI_CPOL_1 : SPI_CPOL_0;
    const spi_cpha_t cpha = (mode & 1) ? SPI_CPHA_1 : SPI_CPHA_0;
    spi_set_format(spi, 8, cpol, cpha, SPI_MSB_FIRST);
    return actual;
#else
    (void)mode;
    return clockHz;
#endif
}

}  // namespace DekiRaspberryPi

#endif  // __has_include("IDekiSPI.h")
