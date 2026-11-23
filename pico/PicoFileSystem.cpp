#include "PicoFileSystem.h"

#include <deki/LogSystem.h>

#include <cstring>

#if defined(DEKI_PICO)
#include <hardware/regs/addressmap.h>

// Where the program ends in flash, from the Pico SDK's linker script.
extern "C" char __flash_binary_end;
#endif

namespace DekiRaspberryPi
{

namespace
{

constexpr char kFlashPrefix[] = "F:/";
constexpr size_t kFlashPrefixLength = sizeof(kFlashPrefix) - 1;

// The name inside the image for `path`: F:/boot.scene and boot.scene are both
// boot.scene. Null for a path of another drive (S:/...).
const char* ImageName(const char* path)
{
    if (!path)
    {
        return nullptr;
    }
    if (std::strncmp(path, kFlashPrefix, kFlashPrefixLength) == 0)
    {
        return path + kFlashPrefixLength;
    }
    if (path[0] != '\0' && path[1] == ':')
    {
        return nullptr;
    }
    return path;
}

}  // namespace

bool PicoFileSystem::Initialize()
{
    // The image is found on first use: the file system is installed by a
    // static constructor, before main() has started the console.
    m_Initialized = true;
    return true;
}

void PicoFileSystem::Shutdown()
{
    for (OpenImageFile& file : m_Files)
    {
        file = OpenImageFile{};
    }
    m_Image = Deki::FileImage{};
    m_MountTried = false;
    m_Initialized = false;
}

void PicoFileSystem::Mount()
{
    if (m_MountTried)
    {
        return;
    }
    m_MountTried = true;
#if defined(DEKI_PICO) && defined(DEKI_PICO_FLASH_SIZE)
    // The builder flashes the image on the first sector after the program
    // (PicoBuilder::GameDataAt), and the linker says where the program ends.
    // The engine runs on without it, so a missing image shows on the console
    // instead of the board stopping at boot.
    constexpr uintptr_t kSector = 4096;
    constexpr uintptr_t kFlashEnd = XIP_BASE + DEKI_PICO_FLASH_SIZE;
    const uintptr_t at = (reinterpret_cast<uintptr_t>(&__flash_binary_end) + kSector - 1) / kSector * kSector;
    const auto* image = reinterpret_cast<const uint8_t*>(at);
    const uint32_t size = at < kFlashEnd ? Deki::FileImage::SizeFromHeader(image, Deki::FileImage::kHeaderSize) : 0;
    if (size == 0 || size > kFlashEnd - at)
    {
        DEKI_LOG_ERROR("The game's data is not in flash at 0x%x. Build and flash the game again.",
                       static_cast<unsigned>(at));
        return;
    }
    if (!MountImage(image, size))
    {
        DEKI_LOG_ERROR("The game's data in flash is damaged. Flash the game again.");
        return;
    }
    DEKI_LOG_INFO("Game data: %u files, %u bytes, in flash", static_cast<unsigned>(m_Image.GetFileCount()),
                  static_cast<unsigned>(size));
#endif
}

bool PicoFileSystem::MountImage(const uint8_t* data, size_t size)
{
    m_MountTried = true;
    return m_Image.Open(data, size);
}

bool PicoFileSystem::Find(const char* path, const uint8_t*& data, size_t& size) const
{
    const char* name = ImageName(path);
    return name && m_Image.IsOpen() && m_Image.Find(name, std::strlen(name), data, size);
}

PicoFileSystem::OpenImageFile* PicoFileSystem::AsOpenFile(FileHandle handle)
{
    for (OpenImageFile& file : m_Files)
    {
        if (handle == &file)
        {
            return file.open ? &file : nullptr;
        }
    }
    return nullptr;
}

Deki::IFileSystem::FileHandle PicoFileSystem::OpenFile(const char* path, OpenMode mode)
{
    Mount();
    if (!m_Initialized || !path)
    {
        return nullptr;
    }
    if (mode == OpenMode::WriteBinary || mode == OpenMode::WriteText)
    {
        DEKI_LOG_WARNING("%s cannot be written: F:/ on a Pico is the game's data, flashed with it. Write to the SD "
                         "card (S:/) instead.",
                         path);
        return nullptr;
    }
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!Find(path, data, size))
    {
        return nullptr;
    }
    for (OpenImageFile& file : m_Files)
    {
        if (!file.open)
        {
            file = OpenImageFile{ data, size, 0, true };
            return static_cast<FileHandle>(&file);
        }
    }
    DEKI_LOG_ERROR("More than %d files of the game's data open at once; %s was not opened", kMaxOpenFiles, path);
    return nullptr;
}

void PicoFileSystem::CloseFile(FileHandle handle)
{
    if (OpenImageFile* file = AsOpenFile(handle))
    {
        *file = OpenImageFile{};
    }
}

size_t PicoFileSystem::ReadFile(FileHandle handle, void* buffer, size_t size)
{
    OpenImageFile* file = AsOpenFile(handle);
    if (!file || !buffer)
    {
        return 0;
    }
    const size_t left = file->size - file->position;
    const size_t count = size < left ? size : left;
    std::memcpy(buffer, file->data + file->position, count);
    file->position += count;
    return count;
}

size_t PicoFileSystem::WriteFile(FileHandle handle, const void* buffer, size_t size)
{
    (void)handle;
    (void)buffer;
    (void)size;
    return 0;
}

long PicoFileSystem::SeekFile(FileHandle handle, long offset, SeekOrigin origin)
{
    OpenImageFile* file = AsOpenFile(handle);
    if (!file)
    {
        return -1;
    }
    long base = 0;
    switch (origin)
    {
        case SeekOrigin::Begin: base = 0; break;
        case SeekOrigin::Current: base = static_cast<long>(file->position); break;
        case SeekOrigin::End: base = static_cast<long>(file->size); break;
    }
    const long target = base + offset;
    if (target < 0 || target > static_cast<long>(file->size))
    {
        return -1;
    }
    file->position = static_cast<size_t>(target);
    return target;
}

long PicoFileSystem::TellFile(FileHandle handle)
{
    const OpenImageFile* file = AsOpenFile(handle);
    return file ? static_cast<long>(file->position) : -1;
}

long PicoFileSystem::GetFileSize(FileHandle handle)
{
    const OpenImageFile* file = AsOpenFile(handle);
    return file ? static_cast<long>(file->size) : -1;
}

bool PicoFileSystem::FileExists(const char* path)
{
    Mount();
    const uint8_t* data = nullptr;
    size_t size = 0;
    return Find(path, data, size);
}

bool PicoFileSystem::ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize)
{
    // A file of the game's data is bytes in flash, with no path of its own.
    (void)virtualPath;
    (void)outBuffer;
    (void)bufferSize;
    return false;
}

}  // namespace DekiRaspberryPi
