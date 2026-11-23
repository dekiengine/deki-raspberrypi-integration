#include "PicoPwmAudio.h"

#include "PwmAudioPlan.h"

#include <initializer_list>

#if defined(DEKI_PICO)
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/pwm.h>
#include <pico/platform.h>
#endif

namespace DekiRaspberryPi
{

#if defined(DEKI_PICO)

namespace
{

// All of it is the interrupt's once Start has set it and before Stop takes
// it back, so nothing here needs a lock.
PicoPwmAudio::FillFn s_Fill = nullptr;
void* s_Context = nullptr;
int16_t s_Block[PicoPwmAudio::kBlockFrames] = {};
int s_At = PicoPwmAudio::kBlockFrames;  // the next sample of the block; at its end, a new block
uint32_t s_Top = 0;
uint s_Slice = 0;
int s_PinLeft = -1;
int s_PinRight = -1;
uint32_t s_Period = 0;  // counts the PWM's periods, to take every kPeriodsPerSample-th
uint32_t s_Rise = 0;    // the first samples climb from nothing to silence's level, not to click
bool s_Running = false;

constexpr uint32_t kRiseSamples = 512;

// The PWM's interrupt number: the SDK names it differently by chip.
#if defined(PWM_DEFAULT_IRQ_NUM)
constexpr uint kPwmIrq = PWM_DEFAULT_IRQ_NUM();
#else
constexpr uint kPwmIrq = PWM_IRQ_WRAP;
#endif

// Once a PWM period, from RAM: the next sample's pulse width, and a new
// block of samples when the last is used up.
void __isr __time_critical_func(OnPwmWrap)()
{
    pwm_clear_irq(s_Slice);
    if (++s_Period % PwmAudio::kPeriodsPerSample != 0)
    {
        return;  // the same sample again
    }
    if (s_At >= PicoPwmAudio::kBlockFrames)
    {
        s_Fill(s_Context, s_Block, PicoPwmAudio::kBlockFrames);
        s_At = 0;
    }
    uint32_t level = PwmAudio::Level(s_Block[s_At++], s_Top);
    if (s_Rise < kRiseSamples)
    {
        level = level * ++s_Rise / kRiseSamples;
    }
    pwm_set_both_levels(s_Slice, static_cast<uint16_t>(level), static_cast<uint16_t>(level));
}

}  // namespace

bool PicoPwmAudio::Start(int pinLeft, int pinRight, int sampleRate, FillFn fill, void* context)
{
    const int first = pinLeft >= 0 ? pinLeft : pinRight;
    if (s_Running || !fill || first < 0 || sampleRate <= 0)
    {
        return false;
    }
    // Both pins on one slice: one counter, one interrupt.
    const uint slice = pwm_gpio_to_slice_num(static_cast<uint>(first));
    if (pinLeft >= 0 && pinRight >= 0 && pwm_gpio_to_slice_num(static_cast<uint>(pinRight)) != slice)
    {
        return false;
    }
    const uint32_t top = PwmAudio::Top(clock_get_hz(clk_sys), static_cast<uint32_t>(sampleRate));
    if (top == 0)
    {
        return false;
    }
    s_Fill = fill;
    s_Context = context;
    s_At = kBlockFrames;
    s_Top = top;
    s_Slice = slice;
    s_PinLeft = pinLeft;
    s_PinRight = pinRight;
    s_Period = 0;
    s_Rise = 0;

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&config, 1);
    pwm_config_set_wrap(&config, static_cast<uint16_t>(top));
    pwm_init(slice, &config, false);
    pwm_set_both_levels(slice, 0, 0);
    for (const int pin : { pinLeft, pinRight })
    {
        if (pin >= 0)
        {
            gpio_set_function(static_cast<uint>(pin), GPIO_FUNC_PWM);
        }
    }
    pwm_clear_irq(slice);
    pwm_set_irq_enabled(slice, true);
    irq_set_exclusive_handler(kPwmIrq, &OnPwmWrap);
    irq_set_enabled(kPwmIrq, true);
    s_Running = true;
    pwm_set_enabled(slice, true);
    return true;
}

void PicoPwmAudio::Stop()
{
    if (!s_Running)
    {
        return;
    }
    irq_set_enabled(kPwmIrq, false);
    pwm_set_irq_enabled(s_Slice, false);
    irq_remove_handler(kPwmIrq, &OnPwmWrap);
    pwm_set_enabled(s_Slice, false);
    for (const int pin : { s_PinLeft, s_PinRight })
    {
        if (pin >= 0)
        {
            gpio_set_function(static_cast<uint>(pin), GPIO_FUNC_NULL);
        }
    }
    s_Running = false;
    s_Fill = nullptr;
}

bool PicoPwmAudio::IsRunning()
{
    return s_Running;
}

#else

// Not a Pico (the editor, the tests): there is no PWM to make sound with.
bool PicoPwmAudio::Start(int, int, int, FillFn, void*)
{
    return false;
}

void PicoPwmAudio::Stop()
{
}

bool PicoPwmAudio::IsRunning()
{
    return false;
}

#endif

}  // namespace DekiRaspberryPi
