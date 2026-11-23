#include "PicoGPIO.h"

#if defined(DEKI_PICO)
#include <hardware/gpio.h>
#include <hardware/sync.h>
#include <pico/time.h>
#endif

namespace DekiRaspberryPi
{

namespace
{

// Edges counted per pin since the last TakeEdges, and when the last came (in
// microseconds since boot). Written by the GPIO interrupt.
volatile uint32_t s_Edges[PicoGPIO::kMaxPins] = {};
volatile uint64_t s_LastEdgeUs[PicoGPIO::kMaxPins] = {};
[[maybe_unused]] bool s_CallbackSet = false;

#if defined(DEKI_PICO)
void OnGpioInterrupt(uint gpio, uint32_t events)
{
    (void)events;
    PicoGPIO::OnEdge(static_cast<int>(gpio));
}

uint32_t EventsFor(DekiGpio::Edge edge)
{
    switch (edge)
    {
        case DekiGpio::Edge::Rising: return GPIO_IRQ_EDGE_RISE;
        case DekiGpio::Edge::Falling: return GPIO_IRQ_EDGE_FALL;
        case DekiGpio::Edge::Both: return GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;
    }
    return GPIO_IRQ_EDGE_RISE;
}
#endif

}  // namespace

void PicoGPIO::OnEdge(int pin)
{
    if (!IsPin(pin))
    {
        return;
    }
    s_Edges[pin] = s_Edges[pin] + 1;
#if defined(DEKI_PICO)
    s_LastEdgeUs[pin] = time_us_64();
#endif
}

void PicoGPIO::SetPull(int pin, DekiGpio::Pull pull)
{
#if defined(DEKI_PICO)
    gpio_set_pulls(static_cast<uint>(pin), pull == DekiGpio::Pull::Up, pull == DekiGpio::Pull::Down);
#else
    (void)pin;
    (void)pull;
#endif
}

bool PicoGPIO::SetOutput(int pin, bool high)
{
    if (!IsPin(pin))
    {
        return false;
    }
#if defined(DEKI_PICO)
    gpio_init(static_cast<uint>(pin));
    // The level first, so the pin never shows the other one.
    gpio_put(static_cast<uint>(pin), high);
    gpio_set_dir(static_cast<uint>(pin), GPIO_OUT);
#else
    (void)high;
#endif
    return true;
}

bool PicoGPIO::SetInput(int pin, DekiGpio::Pull pull)
{
    if (!IsPin(pin))
    {
        return false;
    }
#if defined(DEKI_PICO)
    gpio_init(static_cast<uint>(pin));
    gpio_set_dir(static_cast<uint>(pin), GPIO_IN);
#endif
    SetPull(pin, pull);
    return true;
}

bool PicoGPIO::Write(int pin, bool high)
{
    if (!IsPin(pin))
    {
        return false;
    }
#if defined(DEKI_PICO)
    gpio_put(static_cast<uint>(pin), high);
#else
    (void)high;
#endif
    return true;
}

bool PicoGPIO::Read(int pin)
{
#if defined(DEKI_PICO)
    return IsPin(pin) && gpio_get(static_cast<uint>(pin));
#else
    (void)pin;
    return false;
#endif
}

bool PicoGPIO::CountEdges(int pin, DekiGpio::Edge edge, DekiGpio::Pull pull)
{
    if (!SetInput(pin, pull))
    {
        return false;
    }
    s_Edges[pin] = 0;
#if defined(DEKI_PICO)
    // The SDK has one callback for every pin; set it the first time, then
    // only turn pins on.
    if (!s_CallbackSet)
    {
        gpio_set_irq_enabled_with_callback(static_cast<uint>(pin), EventsFor(edge), true, &OnGpioInterrupt);
        s_CallbackSet = true;
    }
    else
    {
        gpio_set_irq_enabled(static_cast<uint>(pin), EventsFor(edge), true);
    }
#else
    (void)edge;
#endif
    return true;
}

uint32_t PicoGPIO::TakeEdges(int pin)
{
    if (!IsPin(pin))
    {
        return 0;
    }
    // Read and clear with interrupts held off, so no edge is lost between
    // the two.
#if defined(DEKI_PICO)
    const uint32_t saved = save_and_disable_interrupts();
#endif
    const uint32_t count = s_Edges[pin];
    s_Edges[pin] = 0;
#if defined(DEKI_PICO)
    restore_interrupts(saved);
#endif
    return count;
}

void PicoGPIO::StopCounting(int pin)
{
    if (!IsPin(pin))
    {
        return;
    }
#if defined(DEKI_PICO)
    gpio_set_irq_enabled(static_cast<uint>(pin), GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
#endif
    s_Edges[pin] = 0;
}

bool PicoGPIO::WaitForEdge(int pin, uint32_t timeoutMs)
{
    if (!IsPin(pin))
    {
        return false;
    }
#if defined(DEKI_PICO)
    const uint32_t before = s_Edges[pin];
    const uint64_t end = time_us_64() + static_cast<uint64_t>(timeoutMs) * 1000u;
    while (s_Edges[pin] == before)
    {
        if (time_us_64() >= end)
        {
            return false;
        }
        tight_loop_contents();
    }
    return true;
#else
    (void)timeoutMs;
    return false;
#endif
}

bool PicoGPIO::TimeSinceEdge(int pin, uint32_t& microseconds)
{
    if (!IsPin(pin) || s_LastEdgeUs[pin] == 0)
    {
        return false;
    }
#if defined(DEKI_PICO)
    const uint64_t since = time_us_64() - s_LastEdgeUs[pin];
    microseconds = since > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(since);
    return true;
#else
    (void)microseconds;
    return false;
#endif
}

}  // namespace DekiRaspberryPi
