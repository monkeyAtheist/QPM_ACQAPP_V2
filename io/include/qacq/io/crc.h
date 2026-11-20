#pragma once

// Checksums and CRCs met on microcontroller links, in the parametric model of the CRC catalogue
// (width, polynomial, initial value, reflected input and output, final XOR). Every preset is checked
// against the catalogue "check" value, the CRC of the nine ASCII bytes "123456789".

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qacq::io
{
struct CrcSpec
{
    int width = 16; // 1..32 bits
    std::uint32_t polynomial = 0x1021;
    std::uint32_t initial = 0xFFFF;
    bool reflectIn = false;
    bool reflectOut = false;
    std::uint32_t finalXor = 0;
};

// Catalogue presets.
inline constexpr CrcSpec kCrc8Smbus{8, 0x07, 0x00, false, false, 0x00};               // check 0xF4
inline constexpr CrcSpec kCrc8Maxim{8, 0x31, 0x00, true, true, 0x00};                 // 1-Wire, check 0xA1
inline constexpr CrcSpec kCrc16CcittFalse{16, 0x1021, 0xFFFF, false, false, 0x0000};  // check 0x29B1
inline constexpr CrcSpec kCrc16Xmodem{16, 0x1021, 0x0000, false, false, 0x0000};      // check 0x31C3
inline constexpr CrcSpec kCrc16Modbus{16, 0x8005, 0xFFFF, true, true, 0x0000};        // check 0x4B37
inline constexpr CrcSpec kCrc32{32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF}; // Ethernet, zip, check 0xCBF43926

// Table-driven CRC (one table lookup per byte). Incremental: update() as data arrives, value() at the end.
class Crc
{
public:
    explicit Crc(const CrcSpec &spec) noexcept;

    void reset() noexcept;
    void update(std::span<const std::uint8_t> data) noexcept;
    std::uint32_t value() const noexcept;
    const CrcSpec &spec() const noexcept { return m_spec; }

    std::uint32_t compute(std::span<const std::uint8_t> data) noexcept
    {
        reset();
        update(data);
        return value();
    }

private:
    CrcSpec m_spec;
    std::array<std::uint32_t, 256> m_table{};
    std::uint32_t m_register = 0; // reflected register when reflectIn, else left-aligned on width
    std::uint32_t m_mask = 0;
};

// Frame integrity checks offered by the binary decoder.
enum class Checksum
{
    None,
    Sum8,       // sum of the bytes modulo 256
    Xor8,       // XOR of the bytes
    Crc8,       // CRC-8/SMBUS
    Crc8Maxim,  // CRC-8/MAXIM (Dallas 1-Wire)
    Crc16Ccitt, // CRC-16/CCITT-FALSE
    Crc16Xmodem,
    Crc16Modbus,
    Crc32
};

std::string_view checksumName(Checksum checksum) noexcept;
std::size_t checksumSize(Checksum checksum) noexcept; // bytes on the wire: 0, 1, 2 or 4

// Reusable calculator (the CRC table is built once).
class ChecksumCalculator
{
public:
    explicit ChecksumCalculator(Checksum checksum) noexcept;
    std::uint32_t compute(std::span<const std::uint8_t> data) noexcept;
    Checksum type() const noexcept { return m_type; }

private:
    Checksum m_type;
    Crc m_crc;
};

std::uint32_t computeChecksum(Checksum checksum, std::span<const std::uint8_t> data) noexcept;
} // namespace qacq::io
