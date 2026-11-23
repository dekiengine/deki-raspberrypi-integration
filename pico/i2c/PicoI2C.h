#pragma once

#include "IDekiI2C.h"  // from deki-i2c

#include <deki/PackageConfig.h>

#include <string>

namespace DekiRaspberryPi
{

/// An I2C bus on one of the Pico's two I2C blocks.
///
/// The block is the one the pins belong to: on an RP2040 or RP2350, GPIO n
/// is SDA when n is even and SCL when it is odd, on block (n / 2) % 2. Pins
/// of two different blocks are refused. The port I2CBusComponent passes is
/// the number drivers find the bus by in DekiI2C, nothing more.
///
/// Every transfer has a timeout that grows with its length, so a device that
/// holds the clock low or never answers cannot hang the game.
class PicoI2C : public DekiI2c::IDekiI2C
{
public:
    const char* GetPackageId() const override { return "i2c"; }
    const char* GetPackageName() const override { return "I2C (Pico SDK)"; }
    void Configure(const Deki::PackageConfig& config) override;
    bool Initialize() override;
    void Shutdown() override;
    void Update(float) override {}
    Deki::PackageState GetState() const override { return m_State; }
    const char* GetLastError() const override { return m_LastError.c_str(); }

    int GetPort() const override { return m_Port; }
    int GetSdaPin() const override { return m_PinSda; }
    int GetSclPin() const override { return m_PinScl; }
    int GetFrequencyHz() const override { return static_cast<int>(m_FrequencyHz); }

    bool Probe(uint8_t addr) override;
    bool Read(uint8_t addr, uint8_t reg, uint8_t* dst, size_t len) override;
    bool Write(uint8_t addr, uint8_t reg, const uint8_t* src, size_t len) override;
    bool ReadRaw(uint8_t addr, uint8_t* dst, size_t len) override;
    bool WriteRead(uint8_t addr, const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen) override;
    bool WriteRaw(uint8_t addr, const uint8_t* tx, size_t len) override;

    /// The I2C block GPIO `pin` belongs to (0 or 1), or -1 for no pin.
    static int BlockOfPin(int pin) { return pin < 0 ? -1 : (pin / 2) % 2; }

private:
    int m_PinSda = -1;
    int m_PinScl = -1;
    int m_Port = 0;
    int m_Block = -1;
    uint32_t m_FrequencyHz = 400000;
    Deki::PackageState m_State = Deki::PackageState::Uninitialized;
    std::string m_LastError;

    // How long one byte may take, in microseconds: ten bit times at the bus
    // clock, three times over for a device that stretches the clock.
    uint32_t ByteTimeoutUs() const;
    bool Fail(const char* what);
};

}  // namespace DekiRaspberryPi
