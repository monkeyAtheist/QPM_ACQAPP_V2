#include "qacq/io/crc.h"

namespace qacq::io
{
namespace
{
std::uint32_t reflect(std::uint32_t value, int bits) noexcept
{
    std::uint32_t out = 0;
    for (int i = 0; i < bits; ++i)
        if (value & (1u << i)) out |= 1u << (bits - 1 - i);
    return out;
}

std::uint32_t maskOf(int width) noexcept
{
    return width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
}
} // namespace

Crc::Crc(const CrcSpec &spec) noexcept : m_spec(spec)
{
    if (m_spec.width < 1) m_spec.width = 1;
    if (m_spec.width > 32) m_spec.width = 32;
    const int w = m_spec.width;
    m_mask = maskOf(w);
    if (m_spec.reflectIn)
    {
        // Right-shifting register with the reflected polynomial: works for every width.
        const std::uint32_t poly = reflect(m_spec.polynomial & m_mask, w);
        for (std::uint32_t byte = 0; byte < 256; ++byte)
        {
            std::uint32_t r = byte;
            for (int bit = 0; bit < 8; ++bit) r = (r & 1u) ? (r >> 1) ^ poly : r >> 1;
            m_table[byte] = r;
        }
    }
    else
    {
        // Left-shifting register kept left-aligned on 32 bits, so widths below 8 work too.
        const std::uint32_t poly = (m_spec.polynomial & m_mask) << (32 - w);
        for (std::uint32_t byte = 0; byte < 256; ++byte)
        {
            std::uint32_t r = byte << 24;
            for (int bit = 0; bit < 8; ++bit) r = (r & 0x80000000u) ? (r << 1) ^ poly : r << 1;
            m_table[byte] = r;
        }
    }
    reset();
}

void Crc::reset() noexcept
{
    const std::uint32_t init = m_spec.initial & m_mask;
    m_register = m_spec.reflectIn ? reflect(init, m_spec.width) : init << (32 - m_spec.width);
}

void Crc::update(std::span<const std::uint8_t> data) noexcept
{
    std::uint32_t r = m_register;
    if (m_spec.reflectIn)
        for (std::uint8_t b : data) r = (r >> 8) ^ m_table[(r ^ b) & 0xFFu];
    else
        for (std::uint8_t b : data) r = (r << 8) ^ m_table[((r >> 24) ^ b) & 0xFFu];
    m_register = r;
}

std::uint32_t Crc::value() const noexcept
{
    // Register in "reflected" order when reflectIn: bring it back to the natural order, then apply refout.
    std::uint32_t natural = m_spec.reflectIn ? reflect(m_register, m_spec.width) : m_register >> (32 - m_spec.width);
    natural &= m_mask;
    const std::uint32_t out = m_spec.reflectOut ? reflect(natural, m_spec.width) : natural;
    return (out ^ m_spec.finalXor) & m_mask;
}

std::string_view checksumName(Checksum checksum) noexcept
{
    switch (checksum)
    {
        case Checksum::None: return "Aucune";
        case Checksum::Sum8: return "Somme 8 bits";
        case Checksum::Xor8: return "XOR 8 bits";
        case Checksum::Crc8: return "CRC-8/SMBUS";
        case Checksum::Crc8Maxim: return "CRC-8/MAXIM";
        case Checksum::Crc16Ccitt: return "CRC-16/CCITT-FALSE";
        case Checksum::Crc16Xmodem: return "CRC-16/XMODEM";
        case Checksum::Crc16Modbus: return "CRC-16/MODBUS";
        case Checksum::Crc32: return "CRC-32";
    }
    return "?";
}

std::size_t checksumSize(Checksum checksum) noexcept
{
    switch (checksum)
    {
        case Checksum::None: return 0;
        case Checksum::Sum8:
        case Checksum::Xor8:
        case Checksum::Crc8:
        case Checksum::Crc8Maxim: return 1;
        case Checksum::Crc16Ccitt:
        case Checksum::Crc16Xmodem:
        case Checksum::Crc16Modbus: return 2;
        case Checksum::Crc32: return 4;
    }
    return 0;
}

namespace
{
CrcSpec specOf(Checksum checksum) noexcept
{
    switch (checksum)
    {
        case Checksum::Crc8: return kCrc8Smbus;
        case Checksum::Crc8Maxim: return kCrc8Maxim;
        case Checksum::Crc16Xmodem: return kCrc16Xmodem;
        case Checksum::Crc16Modbus: return kCrc16Modbus;
        case Checksum::Crc32: return kCrc32;
        default: return kCrc16CcittFalse;
    }
}
} // namespace

ChecksumCalculator::ChecksumCalculator(Checksum checksum) noexcept : m_type(checksum), m_crc(specOf(checksum)) {}

std::uint32_t ChecksumCalculator::compute(std::span<const std::uint8_t> data) noexcept
{
    switch (m_type)
    {
        case Checksum::None: return 0;
        case Checksum::Sum8:
        {
            std::uint32_t s = 0;
            for (std::uint8_t b : data) s += b;
            return s & 0xFFu;
        }
        case Checksum::Xor8:
        {
            std::uint32_t s = 0;
            for (std::uint8_t b : data) s ^= b;
            return s;
        }
        default: return m_crc.compute(data);
    }
}

std::uint32_t computeChecksum(Checksum checksum, std::span<const std::uint8_t> data) noexcept
{
    return ChecksumCalculator(checksum).compute(data);
}
} // namespace qacq::io
