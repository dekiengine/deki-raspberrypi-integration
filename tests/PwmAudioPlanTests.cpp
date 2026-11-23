// The arithmetic of sound on a PWM pin: the period for a sample rate, and
// the pulse's width for a sample.

#include <gtest/gtest.h>

#include "../pico/audio/PwmAudioPlan.h"

using namespace DekiRaspberryPi::PwmAudio;

TEST(PwmAudioPlan, ThePeriodFitsTheRateTwiceOver)
{
    // 250 MHz, 22050 samples a second: two periods a sample.
    const uint32_t top = Top(250000000, 22050);
    EXPECT_EQ(top, 250000000u / 44100u - 1);
    // And at the Pico's stock clock.
    EXPECT_EQ(Top(125000000, 22050), 125000000u / 44100u - 1);
}

TEST(PwmAudioPlan, ARateThatCannotBeMadeIsRefused)
{
    EXPECT_EQ(Top(125000000, 0), 0u);
    EXPECT_EQ(Top(125000000, 400000), 0u) << "the period would be too coarse for sound";
    EXPECT_EQ(Top(125000000, 500), 0u) << "the period would not fit the 16-bit counter";
}

TEST(PwmAudioPlan, SilenceIsHalfAndTheExtremesAreTheEnds)
{
    const uint32_t top = 5667;
    EXPECT_EQ(Level(0, top), top / 2);
    EXPECT_LE(Level(32767, top), top);
    EXPECT_GE(Level(32767, top), top - 2);
    EXPECT_LE(Level(-32768, top), 1u);
    // Louder is wider, all the way.
    uint16_t before = Level(-32768, top);
    for (int sample = -32000; sample <= 32000; sample += 1000)
    {
        const uint16_t level = Level(static_cast<int16_t>(sample), top);
        EXPECT_GE(level, before);
        before = level;
    }
}
