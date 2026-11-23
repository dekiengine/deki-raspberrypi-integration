#pragma once

// What a Raspberry Pi Pico build runs on the machine the editor is on.
//
// The editor downloads these (pinned archives and SHA-256) into
// ~/.deki-editor/toolchains like any backend's SDK:
//   - Arm's GNU toolchain for bare-metal Arm, arm-none-eabi, the compiler
//   - the Pico SDK, and TinyUSB at the commit the SDK's release pins (the
//     SDK's archive leaves its lib/tinyusb empty), for USB serial
//   - pioasm and picotool, prebuilt by Raspberry Pi, which the SDK's build
//     runs (PIO programs, binary info)
// The rest it cannot install, so it finds them and says what is missing:
//   - CMake and Ninja, which configure and run the build
//   - a host GCC 16 or newer, which compiles the reflection codegen
//     (-freflection) that runs before the game compiles
//
// TinyUSB has a folder of its own rather than the SDK's lib/tinyusb: an
// install clears the component's folder first, so reinstalling the SDK would
// delete it. The build passes PICO_TINYUSB_PATH.
//
// Only the Windows x86_64 archives are pinned so far. On any other host the
// components show as not installable, and the status says so.

#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/TargetBuilder.h>

#include <mutex>
#include <string>
#include <vector>

namespace DekiEditor
{

// The pinned components: id, version and where each installs, under the
// toolchains folder.
inline constexpr const char* kPicoCompilerComponent = "arm-gnu-toolchain-arm-none-eabi";
inline constexpr const char* kPicoCompilerVersion = "15.2.rel1";
inline constexpr const char* kPicoCompilerInstallPath = "arm-gnu-toolchain/15.2.rel1-arm-none-eabi";

inline constexpr const char* kPicoSdkComponent = "pico-sdk";
inline constexpr const char* kPicoSdkVersion = "2.3.1";
inline constexpr const char* kPicoSdkInstallPath = "raspberrypi/pico-sdk-2.3.1";

inline constexpr const char* kPicoTinyUsbComponent = "tinyusb";
inline constexpr const char* kPicoTinyUsbVersion = "0.18.0";
inline constexpr const char* kPicoTinyUsbCommit = "86ad6e56c1700e85f1c5678607a762cfe3aa2f47";
inline constexpr const char* kPicoTinyUsbInstallPath = "raspberrypi/tinyusb-0.18.0";

inline constexpr const char* kPicoPioasmComponent = "pioasm";
inline constexpr const char* kPicoPioasmInstallPath = "raspberrypi/pico-sdk-tools-2.3.1";

inline constexpr const char* kPicoPicotoolComponent = "picotool";
inline constexpr const char* kPicoPicotoolInstallPath = "raspberrypi/picotool-2.3.1";

/// The builder definition the editor's ToolchainComponentManager installs from:
/// the pinned components for this host, or components that cannot be
/// installed and say why on a host without pinned archives.
std::string PicoToolchainDefinitionJson();

/// Whether this host has pinned archives.
bool PicoHostHasPinnedToolchain();

/// One program the build needs from the machine.
struct PicoHostTool
{
    std::string id;           // "cmake", "ninja", "host-gcc"
    std::string displayName;  // "CMake"
    std::string path;         // where it was found; empty when it was not
    std::string version;      // what it reports, where that matters
    bool ok = false;          // found, and new enough
    std::string problem;      // when not ok: what is wrong and what to do
};

/// Finds CMake, Ninja and a host GCC 16+, once, until Rescan(). Looking runs
/// `g++ -dumpversion`, and the Build panel asks for the components often.
class PicoHostTools
{
public:
    std::vector<PicoHostTool> Get() const;
    void Rescan();

    /// The tool with this id, or an empty one (ok false) when it is unknown.
    PicoHostTool Find(const std::string& id) const;

    /// The GCC major version `g++ -dumpversion` printed ("16.1.0" -> 16), or
    /// -1 when it is not a version.
    static int GccMajor(const std::string& dumpVersion);

private:
    mutable std::mutex m_Mutex;
    mutable bool m_Scanned = false;
    mutable std::vector<PicoHostTool> m_Tools;

    static std::vector<PicoHostTool> Scan();
};

/// The first `name` on PATH (with .exe on Windows), or empty.
std::string PicoFindOnPath(const std::string& name);

}  // namespace DekiEditor
