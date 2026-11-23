#include "PicoPresentWorker.h"

#if __has_include("PanelDisplay.h")

#if defined(DEKI_PICO)
#include <hardware/sync.h>
#include <pico/multicore.h>
#include <pico/time.h>
#endif

namespace DekiRaspberryPi
{

namespace
{

#if defined(DEKI_PICO)
// What core 1 runs: set before it is launched, read once it starts.
void (*s_Run)(void*) = nullptr;
void* s_Arg = nullptr;

void Core1Main()
{
    s_Run(s_Arg);
    // The worker was stopped: core 1 sleeps from here on.
    for (;;)
    {
        __wfe();
    }
}

spin_lock_t* s_Lock = nullptr;

spin_lock_t* Lock()
{
    // Claimed on first use, from the drawing core, before the worker starts.
    if (!s_Lock)
    {
        s_Lock = spin_lock_instance(static_cast<uint>(spin_lock_claim_unused(true)));
    }
    return s_Lock;
}
#endif

bool StartWorker(void (*run)(void*), void* arg)
{
#if defined(DEKI_PICO)
    Lock();
    s_Run = run;
    s_Arg = arg;
    multicore_reset_core1();
    multicore_launch_core1(&Core1Main);
    return true;
#else
    (void)run;
    (void)arg;
    return false;
#endif
}

void YieldWorker()
{
    // Core 1 has nothing else to run: there is no one to yield to.
}

uint64_t MicrosNow()
{
#if defined(DEKI_PICO)
    return time_us_64();
#else
    return 0;
#endif
}

void LockPanel()
{
#if defined(DEKI_PICO)
    spin_lock_unsafe_blocking(Lock());
#endif
}

void UnlockPanel()
{
#if defined(DEKI_PICO)
    spin_unlock_unsafe(Lock());
#endif
}

void WaitPanel()
{
#if defined(DEKI_PICO)
    // Woken by the other core's SEV; any other event only makes the caller
    // check its condition once more.
    __wfe();
#endif
}

void WakePanel()
{
#if defined(DEKI_PICO)
    __sev();
#endif
}

const DekiDisplay::PanelDisplay::WorkerBackend kWorker{ &StartWorker, &YieldWorker, &MicrosNow };
const DekiDisplay::PanelSyncHooks kSyncHooks{ &LockPanel, &UnlockPanel, &WaitPanel, &WakePanel };

}  // namespace

const DekiDisplay::PanelDisplay::WorkerBackend& PicoPresentWorker()
{
    return kWorker;
}

const DekiDisplay::PanelSyncHooks& PicoPanelSyncHooks()
{
    return kSyncHooks;
}

}  // namespace DekiRaspberryPi

#endif  // __has_include("PanelDisplay.h")
