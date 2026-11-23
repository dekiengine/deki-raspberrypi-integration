#pragma once

// The Picos a build can be flashed onto, and how a file gets there.
//
// A Pico held in BOOTSEL mode (BOOTSEL pressed while it is plugged in) is a
// USB drive with INFO_UF2.TXT at its top, named RPI-RP2 for an RP2040 and
// RP2350 for an RP2350. A UF2 file copied onto it is written into its flash,
// and the Pico starts it.
//
// A Pico already running a Deki game built with USB serial (stdio "usb" or
// "both") is a serial port instead. Opening that port at 1200 baud makes the
// Pico SDK's USB serial restart the Pico into BOOTSEL, so it is flashed
// without touching the button.

#include <deki-editor/build/TargetBuilder.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

/// A Pico in BOOTSEL mode, as its USB drive.
struct PicoBootDrive
{
    std::string root;     // "E:" on Windows, the mount point elsewhere
    std::string boardId;  // from INFO_UF2.TXT: "RPI-RP2", "RP2350"
};

/// The board id INFO_UF2.TXT names on its "Board-ID:" line, or empty when it
/// has none.
std::string PicoBoardIdFromInfo(const std::string& infoText);

/// The chip a board id is: "rp2040" for RPI-RP2, "rp2350" for RP2350, empty
/// for another.
std::string PicoChipOfBoardId(const std::string& boardId);

/// Those of `roots` that hold INFO_UF2.TXT, in order, with their board ids.
std::vector<PicoBootDrive> PicoBootDrivesAmong(const std::vector<std::string>& roots);

/// The Picos in BOOTSEL mode on this machine: removable drives only on
/// Windows, the places desktops mount them elsewhere.
std::vector<PicoBootDrive> FindPicoBootDrives();

/// The drive's directory: "E:" means E:'s root, not the current directory on E:.
std::filesystem::path PicoDriveRoot(const std::string& root);

/// Whether two ways of writing a drive name the same one ("e:\" and "E:").
bool NamesSameDrive(const std::string& a, const std::string& b);

/// The drive in `now` that is not in `before`: the Pico that has just
/// restarted into BOOTSEL. Empty when there is none, or more than one.
std::string PickNewBootDrive(const std::vector<PicoBootDrive>& before, const std::vector<PicoBootDrive>& now);

/// Opens `port` at 1200 baud and closes it, which restarts a Pico running
/// the Pico SDK's USB serial into BOOTSEL. False, with the reason in `error`,
/// when the port cannot be opened or set.
bool TouchPortAt1200Baud(const std::string& port, std::string& error);

/// Copies the UF2 file `uf2` onto the Pico drive at `root`, calling
/// `onProgress` with 0 to 1 as it goes. The Pico writes it into its flash and
/// restarts, and the drive goes away. False, with the reason in `error`,
/// when the copy fails or is cancelled.
bool CopyUf2ToDrive(const std::filesystem::path& uf2, const std::string& root,
                    const std::function<void(float)>& onProgress, const std::atomic<bool>* cancel, std::string& error);

}  // namespace DekiEditor
