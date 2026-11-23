#pragma once

#include <deki/FileImage.h>
#include <deki/providers/IFileSystem.h>

#include <cstddef>
#include <cstdint>

namespace DekiRaspberryPi
{

/// F:/ on a Pico: the game's data, a read-only file image (Deki::FileImage)
/// flashed on the first sector after the program. The chip maps its
/// flash into the address space (XIP), so the image is read where it lies:
/// opening a file is a binary search and reading it a copy out of flash.
///
/// Nothing on F:/ can be written: a game keeps what it writes on the SD card
/// (S:/), which deki-sdcard routes to its own file system.
class PicoFileSystem : public Deki::IFileSystem
{
public:
    /// The image is found on first use.
    bool Initialize() override;
    void Shutdown() override;

    /// Serves the game's data from the image in `data`, which must stay where
    /// it is. Mount calls it with the flash; tests hand it bytes. False when
    /// `data` is not a well-formed image.
    bool MountImage(const uint8_t* data, size_t size);

    FileHandle OpenFile(const char* path, OpenMode mode) override;
    void CloseFile(FileHandle handle) override;
    size_t ReadFile(FileHandle handle, void* buffer, size_t size) override;
    size_t WriteFile(FileHandle handle, const void* buffer, size_t size) override;
    long SeekFile(FileHandle handle, long offset, SeekOrigin origin) override;
    long TellFile(FileHandle handle) override;
    long GetFileSize(FileHandle handle) override;
    bool FileExists(const char* path) override;
    bool ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize) override;

    /// Files of the game's data open at once.
    static constexpr int kMaxOpenFiles = 16;

private:
    // An open file: where its bytes are and how far it has been read.
    struct OpenImageFile
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t position = 0;
        bool open = false;
    };

    Deki::FileImage m_Image;
    OpenImageFile m_Files[kMaxOpenFiles];
    bool m_Initialized = false;
    bool m_MountTried = false;

    // Finds the image in flash, once.
    void Mount();
    OpenImageFile* AsOpenFile(FileHandle handle);
    // The file `path` names in the image, or false when it has none. Takes
    // F:/ paths and paths with no prefix.
    bool Find(const char* path, const uint8_t*& data, size_t& size) const;
};

}  // namespace DekiRaspberryPi
