#pragma once

#include "FatVolume.h"

#include <deki/providers/IFileSystem.h>

namespace DekiRaspberryPi
{

/// Deki::IFileSystem on a mounted FatVolume, for the SD card.
///
/// Paths, as the ESP-IDF SD card takes them:
/// - "S:/saves/game.sav" -> "saves/game.sav" on the volume ("D:/" too, the old prefix)
/// - "/sdcard/saves/game.sav" (the mount point, as ConvertPath gives it) -> the same file
/// - anything else is a path from the volume's root
///
/// Text and binary modes are the same: there is no line-ending translation.
/// Opening for writing makes the file, or empties it; the directory it goes
/// in must exist.
class FatFileSystem : public Deki::IFileSystem
{
public:
    /// How many files can be open at once. Each one costs 28 bytes.
    static constexpr int kMaxOpenFiles = 4;

    /// `mountPoint` is what ConvertPath puts in front ("/sdcard"); it must
    /// outlive this.
    FatFileSystem(FatVolume& volume, const char* mountPoint);
    ~FatFileSystem() override;

    /// Closes every open file, writing what they hold. For unmounting.
    void CloseAll();

    // Deki::IFileSystem
    bool Initialize() override;
    void Shutdown() override;
    FileHandle OpenFile(const char* path, OpenMode mode) override;
    void CloseFile(FileHandle handle) override;
    size_t ReadFile(FileHandle handle, void* buffer, size_t size) override;
    size_t WriteFile(FileHandle handle, const void* buffer, size_t size) override;
    long SeekFile(FileHandle handle, long offset, SeekOrigin origin) override;
    long TellFile(FileHandle handle) override;
    long GetFileSize(FileHandle handle) override;
    bool FileExists(const char* path) override;
    bool ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize) override;

private:
    FatVolume& m_Volume;
    const char* m_MountPoint;
    FatFile m_Files[kMaxOpenFiles];

    // The path on the volume for a virtual or converted path.
    const char* VolumePath(const char* path) const;

    // The open file behind `handle`, or nullptr when it is not one of ours.
    FatFile* FileFor(FileHandle handle);
};

}  // namespace DekiRaspberryPi
