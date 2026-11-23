#include "PicoDevices.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

constexpr const char* kPicoInfoFile = "INFO_UF2.TXT";

std::string Trimmed(const std::string& s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    const size_t last = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// A drive written one way: forward slashes, no trailing one, and on Windows,
// where drive letters ignore case, lower case.
std::string DriveKey(const std::string& root)
{
    std::string key = PicoDriveRoot(root).lexically_normal().generic_string();
    while (key.size() > 1 && key.back() == '/')
    {
        key.pop_back();
    }
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

}  // namespace

std::string PicoBoardIdFromInfo(const std::string& infoText)
{
    std::istringstream lines(infoText);
    std::string line;
    while (std::getline(lines, line))
    {
        const std::string key = "Board-ID:";
        if (line.rfind(key, 0) == 0)
        {
            return Trimmed(line.substr(key.size()));
        }
    }
    return "";
}

std::string PicoChipOfBoardId(const std::string& boardId)
{
    if (boardId == "RPI-RP2")
    {
        return "rp2040";
    }
    if (boardId == "RP2350")
    {
        return "rp2350";
    }
    return "";
}

fs::path PicoDriveRoot(const std::string& root)
{
#ifdef _WIN32
    if (root.size() == 2 && root[1] == ':')
    {
        return fs::path(root + "\\");
    }
#endif
    return fs::path(root);
}

bool NamesSameDrive(const std::string& a, const std::string& b)
{
    return !a.empty() && !b.empty() && DriveKey(a) == DriveKey(b);
}

std::vector<PicoBootDrive> PicoBootDrivesAmong(const std::vector<std::string>& roots)
{
    std::vector<PicoBootDrive> drives;
    for (const auto& root : roots)
    {
        std::ifstream in(PicoDriveRoot(root) / kPicoInfoFile, std::ios::binary);
        if (!in)
        {
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        drives.push_back({ root, PicoBoardIdFromInfo(text) });
    }
    return drives;
}

std::vector<PicoBootDrive> FindPicoBootDrives()
{
    std::vector<std::string> roots;
#ifdef _WIN32
    // Removable drives only: a fixed disk with the same file is not a Pico.
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if ((mask & (1u << i)) == 0)
        {
            continue;
        }
        const std::string id = std::string(1, static_cast<char>('A' + i)) + ":";
        if (GetDriveTypeA((id + "\\").c_str()) == DRIVE_REMOVABLE)
        {
            roots.push_back(id);
        }
    }
#else
    // Where desktops mount removable media: /media/<user>/<volume>,
    // /run/media/<user>/<volume>, /Volumes/<volume>.
    std::error_code ec;
    for (const char* parent : { "/Volumes", "/media", "/run/media" })
    {
        for (const auto& entry : fs::directory_iterator(parent, ec))
        {
            roots.push_back(entry.path().string());
            std::error_code inner;
            for (const auto& nested : fs::directory_iterator(entry.path(), inner))
            {
                roots.push_back(nested.path().string());
            }
        }
    }
#endif
    return PicoBootDrivesAmong(roots);
}

std::string PickNewBootDrive(const std::vector<PicoBootDrive>& before, const std::vector<PicoBootDrive>& now)
{
    std::vector<std::string> fresh;
    for (const auto& drive : now)
    {
        const bool wasThere = std::any_of(before.begin(), before.end(),
                                          [&](const PicoBootDrive& b) { return NamesSameDrive(b.root, drive.root); });
        if (!wasThere)
        {
            fresh.push_back(drive.root);
        }
    }
    return fresh.size() == 1 ? fresh.front() : std::string();
}

bool TouchPortAt1200Baud(const std::string& port, std::string& error)
{
#ifdef _WIN32
    // \\.\ reaches COM10 and above, which the plain name does not.
    const std::string path = "\\\\.\\" + port;
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = "cannot open " + port + " (Windows error " + std::to_string(GetLastError()) +
                "); is another program, such as the Serial Monitor, using it?";
        return false;
    }
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    bool ok = GetCommState(handle, &dcb) != 0;
    if (ok)
    {
        dcb.BaudRate = 1200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        ok = SetCommState(handle, &dcb) != 0;
    }
    if (!ok)
    {
        error = "cannot set " + port + " to 1200 baud (Windows error " + std::to_string(GetLastError()) + ")";
    }
    CloseHandle(handle);
    return ok;
#else
    const int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        error = "cannot open " + port + ": " + std::strerror(errno);
        return false;
    }
    termios tty = {};
    bool ok = tcgetattr(fd, &tty) == 0;
    if (ok)
    {
        cfmakeraw(&tty);
        cfsetispeed(&tty, B1200);
        cfsetospeed(&tty, B1200);
        ok = tcsetattr(fd, TCSANOW, &tty) == 0;
    }
    if (!ok)
    {
        error = "cannot set " + port + " to 1200 baud: " + std::strerror(errno);
    }
    close(fd);
    return ok;
#endif
}

bool CopyUf2ToDrive(const fs::path& uf2, const std::string& root, const std::function<void(float)>& onProgress,
                    const std::atomic<bool>* cancel, std::string& error)
{
    std::ifstream in(uf2, std::ios::binary);
    if (!in)
    {
        error = "cannot read " + uf2.string() + "; build first";
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const fs::path target = PicoDriveRoot(root) / uf2.filename();
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "cannot write " + target.string();
        return false;
    }

    // In pieces, so the progress moves: the Pico writes its flash as the
    // blocks arrive, at a few hundred KB a second.
    constexpr size_t kChunk = 64 * 1024;
    bool lastChunkSent = false;
    for (size_t at = 0; at < bytes.size(); at += kChunk)
    {
        if (cancel != nullptr && *cancel)
        {
            error = "cancelled; the Pico is still in BOOTSEL mode, so flash again to finish";
            return false;
        }
        const size_t n = std::min(kChunk, bytes.size() - at);
        lastChunkSent = at + n == bytes.size();
        out.write(bytes.data() + at, static_cast<std::streamsize>(n));
        out.flush();
        if (!out)
        {
            break;
        }
        if (onProgress)
        {
            onProgress(static_cast<float>(at + n) / static_cast<float>(bytes.size()));
        }
    }
    out.close();
    if (!out)
    {
        // The Pico restarts as soon as its last block is in, and its drive
        // can go before the file is closed: that is a finished copy.
        std::error_code ec;
        if (lastChunkSent && !fs::exists(PicoDriveRoot(root) / kPicoInfoFile, ec))
        {
            return true;
        }
        error = "writing " + target.string() + " failed";
        return false;
    }
    return true;
}

}  // namespace DekiEditor
