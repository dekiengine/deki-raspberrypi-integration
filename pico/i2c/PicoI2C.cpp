#include "PicoI2C.h"

#include <deki/LogSystem.h>
#include <deki/providers/Buffer.h>

#include <cstring>

#if defined(DEKI_PICO)
#include <hardware/gpio.h>
#include <hardware/i2c.h>
#endif

namespace DekiRaspberryPi
{

namespace
{

// A register write up to this long is built on the stack; a longer one in a
// buffer of its own.
constexpr size_t kStackWriteBytes = 32;

#if defined(DEKI_PICO)
i2c_inst_t* Block(int block)
{
    return block == 1 ? i2c1 : i2c0;
}
#endif

}  // namespace

void PicoI2C::Configure(const Deki::PackageConfig& config)
{
    m_PinSda = config.GetPin("SDA", -1);
    m_PinScl = config.GetPin("SCL", -1);
    m_Port = config.GetInt("i2cPort", 0);
    const int hz = config.GetInt("i2cHz", 400000);
    m_FrequencyHz = hz > 0 ? static_cast<uint32_t>(hz) : 400000;
}

bool PicoI2C::Fail(const char* what)
{
    m_LastError = what;
    m_State = Deki::PackageState::Error;
    return false;
}

bool PicoI2C::Initialize()
{
    if (m_PinSda < 0 || m_PinScl < 0)
    {
        return Fail("PicoI2C: SDA and SCL pins are both needed");
    }
    if (m_PinSda % 2 != 0 || m_PinScl % 2 != 1)
    {
        return Fail("PicoI2C: SDA must be an even GPIO and SCL an odd one");
    }
    if (BlockOfPin(m_PinSda) != BlockOfPin(m_PinScl))
    {
        return Fail("PicoI2C: SDA and SCL belong to different I2C blocks");
    }
    m_Block = BlockOfPin(m_PinSda);
#if defined(DEKI_PICO)
    const uint32_t actual = i2c_init(Block(m_Block), m_FrequencyHz);
    gpio_set_function(static_cast<uint>(m_PinSda), GPIO_FUNC_I2C);
    gpio_set_function(static_cast<uint>(m_PinScl), GPIO_FUNC_I2C);
    // The chip's own pull-ups (about 50 kOhm) for a board with none of its
    // own; with board pull-ups they change nothing.
    gpio_pull_up(static_cast<uint>(m_PinSda));
    gpio_pull_up(static_cast<uint>(m_PinScl));
    DEKI_LOG_INFO("PicoI2C: i2c%d on SDA %d, SCL %d at %u Hz", m_Block, m_PinSda, m_PinScl,
                  static_cast<unsigned>(actual));
#endif
    m_State = Deki::PackageState::Initialized;
    return true;
}

void PicoI2C::Shutdown()
{
#if defined(DEKI_PICO)
    if (m_State == Deki::PackageState::Initialized)
    {
        i2c_deinit(Block(m_Block));
    }
#endif
    m_State = Deki::PackageState::Uninitialized;
}

uint32_t PicoI2C::ByteTimeoutUs() const
{
    const uint32_t bitUs = 1000000u / (m_FrequencyHz > 0 ? m_FrequencyHz : 1);
    return 30u * bitUs + 1000u;
}

bool PicoI2C::Probe(uint8_t addr)
{
    uint8_t byte = 0;
    return ReadRaw(addr, &byte, 1);
}

bool PicoI2C::Read(uint8_t addr, uint8_t reg, uint8_t* dst, size_t len)
{
    return WriteRead(addr, &reg, 1, dst, len);
}

bool PicoI2C::Write(uint8_t addr, uint8_t reg, const uint8_t* src, size_t len)
{
    // The register byte and the data go out as one transaction.
    if (len + 1 <= kStackWriteBytes)
    {
        uint8_t bytes[kStackWriteBytes];
        bytes[0] = reg;
        if (len > 0)
        {
            std::memcpy(bytes + 1, src, len);
        }
        return WriteRaw(addr, bytes, len + 1);
    }
    Deki::Buffer<uint8_t> bytes(len + 1, Deki::Memory::Internal);
    if (!bytes)
    {
        return false;
    }
    bytes[0] = reg;
    std::memcpy(bytes.Data() + 1, src, len);
    return WriteRaw(addr, bytes.Data(), len + 1);
}

bool PicoI2C::ReadRaw(uint8_t addr, uint8_t* dst, size_t len)
{
    if (m_State != Deki::PackageState::Initialized || !dst || len == 0)
    {
        return false;
    }
#if defined(DEKI_PICO)
    const int got = i2c_read_timeout_per_char_us(Block(m_Block), addr, dst, len, false, ByteTimeoutUs());
    return got == static_cast<int>(len);
#else
    (void)addr;
    return false;
#endif
}

bool PicoI2C::WriteRaw(uint8_t addr, const uint8_t* tx, size_t len)
{
    if (m_State != Deki::PackageState::Initialized || !tx || len == 0)
    {
        return false;
    }
#if defined(DEKI_PICO)
    const int sent = i2c_write_timeout_per_char_us(Block(m_Block), addr, tx, len, false, ByteTimeoutUs());
    return sent == static_cast<int>(len);
#else
    (void)addr;
    return false;
#endif
}

bool PicoI2C::WriteRead(uint8_t addr, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen)
{
    if (m_State != Deki::PackageState::Initialized || !tx || txLen == 0 || !rx || rxLen == 0)
    {
        return false;
    }
#if defined(DEKI_PICO)
    // No stop after the write: the read follows with a repeated start.
    i2c_inst_t* block = Block(m_Block);
    if (i2c_write_timeout_per_char_us(block, addr, tx, txLen, true, ByteTimeoutUs()) != static_cast<int>(txLen))
    {
        return false;
    }
    return i2c_read_timeout_per_char_us(block, addr, rx, rxLen, false, ByteTimeoutUs()) == static_cast<int>(rxLen);
#else
    (void)addr;
    return false;
#endif
}

}  // namespace DekiRaspberryPi
