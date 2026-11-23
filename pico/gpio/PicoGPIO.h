#pragma once

#include "IDekiGPIO.h"  // from deki-gpio

#include <cstdint>

namespace DekiRaspberryPi
{

/// GPIO pins on a Pico.
///
/// Edge counting uses the chip's one GPIO interrupt: its handler adds to the
/// pin's counter and notes when the edge came. A counter is a 32-bit word,
/// which the Cortex-M core reads and writes in one go, so the handler and the
/// frame need no lock.
class PicoGPIO : public DekiGpio::IDekiGPIO
{
public:
    bool SetOutput(int pin, bool high) override;
    bool SetInput(int pin, DekiGpio::Pull pull) override;
    bool Write(int pin, bool high) override;
    bool Read(int pin) override;

    bool CountEdges(int pin, DekiGpio::Edge edge, DekiGpio::Pull pull) override;
    uint32_t TakeEdges(int pin) override;
    void StopCounting(int pin) override;
    bool WaitForEdge(int pin, uint32_t timeoutMs) override;
    bool TimeSinceEdge(int pin, uint32_t& microseconds) override;

    /// GPIOs this class handles: 48 covers the RP2350B, the largest.
    static constexpr int kMaxPins = 48;

    /// Called from the GPIO interrupt for an edge on `pin`.
    static void OnEdge(int pin);

private:
    static bool IsPin(int pin) { return pin >= 0 && pin < kMaxPins; }
    void SetPull(int pin, DekiGpio::Pull pull);
};

}  // namespace DekiRaspberryPi
