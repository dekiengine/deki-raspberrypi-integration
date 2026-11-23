#pragma once

// A new build of the game sent over USB while the game runs, with no one
// touching the board: tools/pico-usb-update.ps1 sends it, the game takes it,
// and the board restarts into it.
//
// Only for games behind a boot loader (frameworkOptions loaderOffset, the
// PicoCalc's menu): the loader is never touched, so a failed update at worst
// leaves the old game, or one to install again from the loader. The new image
// is written beside the running one first and checked; only then, from code
// in RAM with everything else stopped, copied over it.

namespace DekiRaspberryPi
{

/// Listens for an update every frame, when the game has USB serial and sits
/// behind a boot loader. Call once, before the engine runs.
void StartUsbUpdates();

}  // namespace DekiRaspberryPi
