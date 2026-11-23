#include "FatFileSystem.h"

#include <deki/LogSystem.h>

#include <cstdio>
#include <cstring>

namespace DekiRaspberryPi
{

namespace
{

bool HasDrivePrefix(const char* path)
{
    const char drive = path[0];
    return (drive == 'S' || drive == 's' || drive == 'D' || drive == 'd') && path[1] == ':' && path[2] == '/';
}

}  // namespace

FatFileSystem::FatFileSystem(FatVolume& volume, const char* mountPoint)
    : m_Volume(volume),
      m_MountPoint(mountPoint != nullptr ? mountPoint : "")
{
}

FatFileSystem::~FatFileSystem()
{
    CloseAll();
}

void FatFileSystem::CloseAll()
{
    for (FatFile& file : m_Files)
    {
        if ((file.flags & kFatFileOpen) != 0)
        {
            m_Volume.Close(file);
        }
    }
}

bool FatFileSystem::Initialize()
{
    // PicoSDCard mounts the volume.
    return true;
}

void FatFileSystem::Shutdown()
{
    CloseAll();
}

const char* FatFileSystem::VolumePath(const char* path) const
{
    if (HasDrivePrefix(path))
    {
        return path + 3;
    }
    const size_t mountLength = std::strlen(m_MountPoint);
    if (mountLength > 0 && std::strncmp(path, m_MountPoint, mountLength) == 0 &&
        (path[mountLength] == '/' || path[mountLength] == '\0'))
    {
        return path + mountLength;
    }
    return path;
}

FatFile* FatFileSystem::FileFor(FileHandle handle)
{
    for (FatFile& file : m_Files)
    {
        if (static_cast<FileHandle>(&file) == handle && (file.flags & kFatFileOpen) != 0)
        {
            return &file;
        }
    }
    return nullptr;
}

Deki::IFileSystem::FileHandle FatFileSystem::OpenFile(const char* path, OpenMode mode)
{
    if (path == nullptr || !m_Volume.IsMounted())
    {
        return nullptr;
    }

    FatFile* slot = nullptr;
    for (FatFile& file : m_Files)
    {
        if ((file.flags & kFatFileOpen) == 0)
        {
            slot = &file;
            break;
        }
    }
    if (slot == nullptr)
    {
        DEKI_LOG_WARNING("FatFileSystem: %d files are open already; %s not opened", kMaxOpenFiles, path);
        return nullptr;
    }

    const bool write = mode == OpenMode::WriteBinary || mode == OpenMode::WriteText;
    if (!m_Volume.Open(*slot, VolumePath(path), write ? FatOpenMode::Write : FatOpenMode::Read))
    {
        return nullptr;
    }
    return static_cast<FileHandle>(slot);
}

void FatFileSystem::CloseFile(FileHandle handle)
{
    FatFile* file = FileFor(handle);
    if (file != nullptr && !m_Volume.Close(*file))
    {
        DEKI_LOG_ERROR("FatFileSystem: writing a file to the SD card failed");
    }
}

size_t FatFileSystem::ReadFile(FileHandle handle, void* buffer, size_t size)
{
    FatFile* file = FileFor(handle);
    return file != nullptr && buffer != nullptr ? m_Volume.Read(*file, buffer, size) : 0;
}

size_t FatFileSystem::WriteFile(FileHandle handle, const void* buffer, size_t size)
{
    FatFile* file = FileFor(handle);
    return file != nullptr && buffer != nullptr ? m_Volume.Write(*file, buffer, size) : 0;
}

long FatFileSystem::SeekFile(FileHandle handle, long offset, SeekOrigin origin)
{
    FatFile* file = FileFor(handle);
    if (file == nullptr)
    {
        return -1;
    }

    long long base = 0;
    switch (origin)
    {
        case SeekOrigin::Begin: base = 0; break;
        case SeekOrigin::Current: base = file->position; break;
        case SeekOrigin::End: base = file->size; break;
    }
    const long long target = base + offset;
    if (target < 0 || target > 0xFFFFFFFFLL)
    {
        return -1;
    }
    m_Volume.Seek(*file, static_cast<uint32_t>(target));
    return static_cast<long>(target);
}

long FatFileSystem::TellFile(FileHandle handle)
{
    FatFile* file = FileFor(handle);
    return file != nullptr ? static_cast<long>(file->position) : -1;
}

long FatFileSystem::GetFileSize(FileHandle handle)
{
    FatFile* file = FileFor(handle);
    return file != nullptr ? static_cast<long>(file->size) : -1;
}

bool FatFileSystem::FileExists(const char* path)
{
    return path != nullptr && m_Volume.Exists(VolumePath(path));
}

bool FatFileSystem::ConvertPath(const char* virtualPath, char* outBuffer, size_t bufferSize)
{
    if (virtualPath == nullptr || outBuffer == nullptr || bufferSize == 0)
    {
        return false;
    }
    int written = 0;
    if (HasDrivePrefix(virtualPath))
    {
        written = std::snprintf(outBuffer, bufferSize, "%s/%s", m_MountPoint, virtualPath + 3);
    }
    else
    {
        written = std::snprintf(outBuffer, bufferSize, "%s", virtualPath);
    }
    return written >= 0 && static_cast<size_t>(written) < bufferSize;
}

}  // namespace DekiRaspberryPi
