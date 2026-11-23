#include "UsbUpdatePlan.h"

#include <cstdlib>
#include <cstring>

namespace DekiRaspberryPi::UsbUpdate
{

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size)
{
    // Bit by bit: no table to keep in RAM, and a whole image takes a
    // fraction of a second even on an RP2040.
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool ParseRequest(const char* line, uint32_t& size, uint32_t& crc)
{
    const size_t length = sizeof(kCommand) - 1;
    if (std::strncmp(line, kCommand, length) != 0 || line[length] != ' ')
    {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsedSize = std::strtoul(line + length + 1, &end, 10);
    if (end == line + length + 1 || *end != ' ' || parsedSize == 0)
    {
        return false;
    }
    const char* crcText = end + 1;
    const unsigned long parsedCrc = std::strtoul(crcText, &end, 16);
    if (end == crcText || (*end != 0 && *end != '\r' && *end != '\n' && *end != ' '))
    {
        return false;
    }
    size = static_cast<uint32_t>(parsedSize);
    crc = static_cast<uint32_t>(parsedCrc);
    return true;
}

uint32_t AlignUp(uint32_t value)
{
    return (value + kSector - 1) / kSector * kSector;
}

bool PlanStaging(uint32_t imageEnd, uint32_t size, uint32_t flashEnd, uint32_t& staging)
{
    const uint32_t from = AlignUp(imageEnd);
    if (size == 0 || from > flashEnd || AlignUp(size) > flashEnd - from)
    {
        return false;
    }
    staging = from;
    return true;
}

}  // namespace DekiRaspberryPi::UsbUpdate
