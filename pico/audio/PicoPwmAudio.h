#pragma once

#include <cstdint>

namespace DekiRaspberryPi
{

/// Sound out of one or two PWM pins: a speaker behind a filter and an
/// amplifier, as on the PicoCalc.
///
/// The PWM's own interrupt is the sample clock. A few samples at a time it
/// asks `fill` for more, from the interrupt, on the core that called Start:
/// so `fill` must be short, must not allocate or block, and what it shares
/// with the game must be safe to touch from an interrupt. Nothing here
/// waits on the game's frame: the sound goes on through a slow one.
///
/// Mono: both pins carry the same signal. A board gives its pins (see its
/// own package); this class knows none.
class PicoPwmAudio
{
public:
    /// Fills `out` with `frames` signed 16-bit samples. Called from the
    /// interrupt.
    using FillFn = void (*)(void* context, int16_t* out, int frames);

    /// Samples asked for at a time: 3 ms of sound at 22050 a second.
    static constexpr int kBlockFrames = 64;

    /// Starts sound on `pinLeft` and `pinRight` (the same, or -1 for one
    /// that is not there) at `sampleRate` samples a second. False when the
    /// pins are not PWM pins of one slice, the rate cannot be made, or it
    /// is already running.
    static bool Start(int pinLeft, int pinRight, int sampleRate, FillFn fill, void* context);

    /// Stops it: the pins go quiet and are let go.
    static void Stop();

    static bool IsRunning();
};

}  // namespace DekiRaspberryPi
