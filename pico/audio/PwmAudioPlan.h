#pragma once

// The arithmetic of sound on a PWM pin, apart from the hardware so it can be
// tested on a PC: how long the PWM's period is for a sample rate, and how
// wide the pulse is for a sample.

#include <cstdint>

namespace DekiRaspberryPi::PwmAudio
{

/// The PWM carries each sample for this many of its periods: with two, its
/// own tone is at twice the sample rate, out of hearing.
constexpr int kPeriodsPerSample = 2;

/// The counter's top for `sampleRate` samples a second on a `clockHz` system
/// clock (the period is top + 1 ticks). 0 when the rate cannot be made: the
/// counter is 16 bits wide, and a period under 256 ticks is too coarse to
/// carry sound.
constexpr uint32_t Top(uint32_t clockHz, uint32_t sampleRate)
{
    if (sampleRate == 0)
    {
        return 0;
    }
    const uint32_t ticks = clockHz / (sampleRate * kPeriodsPerSample);
    return ticks >= 256 && ticks <= 65536 ? ticks - 1 : 0;
}

/// The pulse's width for a signed 16-bit `sample`: half of `top` for
/// silence, 0 and `top` at the extremes.
constexpr uint16_t Level(int16_t sample, uint32_t top)
{
    const int32_t middle = static_cast<int32_t>(top / 2);
    const int32_t level = middle + (static_cast<int32_t>(sample) * static_cast<int32_t>(top + 1)) / 65536;
    return static_cast<uint16_t>(level < 0 ? 0
                                           : (level > static_cast<int32_t>(top) ? static_cast<int32_t>(top) : level));
}

}  // namespace DekiRaspberryPi::PwmAudio
