// The rules of an update over USB: the request line, the checksum (the one
// the host computes too), and where the new image waits in flash.

#include <gtest/gtest.h>

#include "../pico/UsbUpdatePlan.h"

#include <cstring>
#include <vector>

using namespace DekiRaspberryPi::UsbUpdate;

TEST(UsbUpdatePlan, TheChecksumIsZlibs)
{
    const char* text = "123456789";
    EXPECT_EQ(Crc32(0, reinterpret_cast<const uint8_t*>(text), std::strlen(text)), 0xCBF43926u);
    // Carried on in pieces, the same.
    const uint32_t first = Crc32(0, reinterpret_cast<const uint8_t*>(text), 4);
    EXPECT_EQ(Crc32(first, reinterpret_cast<const uint8_t*>(text) + 4, 5), 0xCBF43926u);
    EXPECT_EQ(Crc32(0, nullptr, 0), 0u);
}

TEST(UsbUpdatePlan, TheRequestIsReadOrRefused)
{
    uint32_t size = 0;
    uint32_t crc = 0;
    ASSERT_TRUE(ParseRequest("DEKI-UPDATE 838784 cbf43926", size, crc));
    EXPECT_EQ(size, 838784u);
    EXPECT_EQ(crc, 0xCBF43926u);
    EXPECT_TRUE(ParseRequest("DEKI-UPDATE 5 00000001\r", size, crc));
    EXPECT_FALSE(ParseRequest("DEKI-UPDATE", size, crc));
    EXPECT_FALSE(ParseRequest("DEKI-UPDATE 0 1234", size, crc));
    EXPECT_FALSE(ParseRequest("DEKI-UPDATE abc 1234", size, crc));
    EXPECT_FALSE(ParseRequest("DEKI-UPDATE 12 xyz", size, crc));
    EXPECT_FALSE(ParseRequest("DEKI-UPDATEX 12 34", size, crc));
    EXPECT_FALSE(ParseRequest("hello", size, crc));
}

TEST(UsbUpdatePlan, TheNewImageWaitsAfterTheRunningOne)
{
    constexpr uint32_t kFlash = 2 * 1024 * 1024;
    uint32_t staging = 0;
    // The PicoCalc's game: from 200 KB, about 840 KB long.
    ASSERT_TRUE(PlanStaging(204800 + 838784, 838784, kFlash, staging));
    EXPECT_EQ(staging % kSector, 0u);
    EXPECT_GE(staging, 204800u + 838784u);
    EXPECT_LE(staging + AlignUp(838784), kFlash);
    // Too big to fit beside it.
    EXPECT_FALSE(PlanStaging(204800 + 1000000, 1000000, kFlash, staging));
    EXPECT_FALSE(PlanStaging(kFlash + 1, 10, kFlash, staging));
    EXPECT_FALSE(PlanStaging(1000, 0, kFlash, staging));
    // Exactly fitting.
    EXPECT_TRUE(PlanStaging(kFlash - 8192, 8192, kFlash, staging));
    EXPECT_EQ(staging, kFlash - 8192);
}

TEST(UsbUpdatePlan, AlignUpRoundsToASector)
{
    EXPECT_EQ(AlignUp(0), 0u);
    EXPECT_EQ(AlignUp(1), kSector);
    EXPECT_EQ(AlignUp(kSector), kSector);
    EXPECT_EQ(AlignUp(kSector + 1), 2 * kSector);
}

namespace
{

// Flash as a block of bytes: erasing leaves 0xFF, programming writes.
struct FakeFlash
{
    std::vector<uint8_t> bytes = std::vector<uint8_t>(2 * 1024 * 1024, 0xFF);

    void Read(uint32_t offset, uint8_t* out, uint32_t n) { std::memcpy(out, bytes.data() + offset, n); }
    void Erase(uint32_t offset, uint32_t n) { std::memset(bytes.data() + offset, 0xFF, n); }
    void Program(uint32_t offset, const uint8_t* data, uint32_t n) { std::memcpy(bytes.data() + offset, data, n); }
};

}  // namespace

TEST(UsbUpdatePlan, ALongerImageIsCopiedWholeOverItsOwnStaging)
{
    // The PicoCalc on 2026-11-22: the game at 200 KB, waiting at 1 MB, and a
    // new build of 941105 bytes, which reaches past 1 MB once in place.
    // Erasing the whole destination first wiped the start of the new build.
    constexpr uint32_t kApp = 204800;
    constexpr uint32_t kStaging = 1024 * 1024;
    constexpr uint32_t kSize = 941105;
    ASSERT_GT(kApp + kSize, kStaging);
    FakeFlash flash;
    std::vector<uint8_t> image(kSize);
    for (uint32_t i = 0; i < kSize; ++i)
    {
        image[i] = static_cast<uint8_t>(i * 7 + i / 4096);
    }
    std::memcpy(flash.bytes.data() + kStaging, image.data(), kSize);
    uint8_t buffer[kSector];
    CopyImage(flash, kStaging, kApp, kSize, buffer);
    EXPECT_EQ(std::memcmp(flash.bytes.data() + kApp, image.data(), kSize), 0);
    EXPECT_EQ(Crc32(0, flash.bytes.data() + kApp, kSize), Crc32(0, image.data(), kSize));
}
