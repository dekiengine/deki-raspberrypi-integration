#pragma once

// deki-display's present worker on a Pico. Installing this package does not
// bring deki-display: a project without it builds nothing here.
#if __has_include("PanelDisplay.h")

#include "PanelDisplay.h"  // from deki-display
#include "PanelSync.h"     // from deki-display

namespace DekiRaspberryPi
{

/// Runs deki-display's present worker on the Pico's second core, so a frame
/// is sent while the next is drawn: the second core waits for the SPI DMA,
/// the first draws. Core 1 runs nothing else in a Deki game.
const DekiDisplay::PanelDisplay::WorkerBackend& PicoPresentWorker();

/// The lock and wake-ups between the two cores for deki-display: one of the
/// chip's hardware spinlocks, and the cores' wait-for-event (WFE), woken by
/// the other core's SEV.
const DekiDisplay::PanelSyncHooks& PicoPanelSyncHooks();

}  // namespace DekiRaspberryPi

#endif  // __has_include("PanelDisplay.h")
