#pragma once

// Binary samples.
//
// RawBinaryDecoder: a bare stream of samples, channels interleaved (what the old application read).
// Nothing in the stream marks where a frame starts: a lost byte shifts every following value. Kept for
// sources that guarantee the alignment (a file, a sound card, TCP from the start of the stream).
//
// FramedBinaryDecoder: fixed-length frames, the robust choice for a microcontroller link:
//   [sync 1..8 bytes][counter 0, 1 or 2 bytes][payload: samplesPerChannel x channels samples][checksum]
// The payload is frame-major: sample s of channel c at index s * channels + c. Multi-byte fields
// (counter, samples, checksum) use the byte order of the format. The checksum covers the counter and the
// payload, plus the sync bytes if checksumCoversSync. The decoder looks for the sync pattern, checks the
// checksum, and on a mismatch moves on by one byte: it finds the next good frame after any corruption.
// Without a checksum a frame is accepted only once the next frame also starts with the sync pattern
// (after a start or a loss of synchronisation), so an occasional sync-like value inside the data cannot
// lock it. (Data that repeat the sync pattern at the same place in every frame make the framing ambiguous
// whatever the decoder: use a checksum.)
// The counter (wrapping at 2^8 or 2^16) reveals lost frames: counted and marked as a discontinuity.
// Without a counter, every loss of synchronisation is marked as a discontinuity.

#include "qacq/io/crc.h"
#include "qacq/io/decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::io
{
enum class SampleType
{
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Float32,
    Float64
};

enum class ByteOrder
{
    LittleEndian, // AVR, ARM Cortex-M, x86: the usual case
    BigEndian     // network order, many instruments
};

std::string_view sampleTypeName(SampleType type) noexcept;
std::size_t sampleSize(SampleType type) noexcept;

// value = raw * scale + offset (for instance ADC counts -> volts: scale = Vref / 2^bits).
struct SampleFormat
{
    SampleType type = SampleType::Int16;
    ByteOrder order = ByteOrder::LittleEndian;
    double scale = 1.0;
    double offset = 0.0;
};

double readSample(const std::uint8_t *bytes, const SampleFormat &format) noexcept;
// Inverse: rounds and saturates for integer types.
void writeSample(double value, const SampleFormat &format, std::uint8_t *bytes) noexcept;

class RawBinaryDecoder final : public Decoder
{
public:
    explicit RawBinaryDecoder(const SampleFormat &format = {}, std::size_t channels = 1);

    void decode(std::span<const std::uint8_t> bytes, SampleBlock &out) override;
    void reset() override { m_pending.clear(); }
    std::size_t channelCount() const noexcept override { return m_channels; }
    std::size_t pendingBytes() const noexcept override { return m_pending.size(); }

private:
    SampleFormat m_format;
    std::size_t m_channels;
    std::vector<std::uint8_t> m_pending; // less than one frame
};

enum class CounterField
{
    None,
    UInt8,
    UInt16
};

struct FrameFormat
{
    std::vector<std::uint8_t> sync{0xAA, 0x55};
    CounterField counter = CounterField::UInt8;
    SampleFormat sample;
    std::size_t channels = 1;
    std::size_t samplesPerChannel = 1;
    Checksum checksum = Checksum::Crc16Ccitt;
    bool checksumCoversSync = false;
};

std::size_t frameSize(const FrameFormat &format) noexcept;
// Empty when the format is usable, else the reason (in French, for the interface).
std::string validateFrameFormat(const FrameFormat &format);

// Builds one frame: values are frame-major (samplesPerChannel x channels), in physical units.
// For tests, simulated devices and the firmware example shown to the user.
std::vector<std::uint8_t> encodeFrame(const FrameFormat &format, std::uint32_t counter, std::span<const double> values);

class FramedBinaryDecoder final : public Decoder
{
public:
    explicit FramedBinaryDecoder(const FrameFormat &format = {});

    void decode(std::span<const std::uint8_t> bytes, SampleBlock &out) override;
    void reset() override;
    std::size_t channelCount() const noexcept override { return m_format.channels; }
    std::size_t pendingBytes() const noexcept override { return m_pending.size() - m_read; }

    const FrameFormat &format() const noexcept { return m_format; }
    const std::string &formatError() const noexcept { return m_error; }
    bool locked() const noexcept { return m_locked; }

private:
    bool syncAt(std::size_t position) const noexcept;
    bool checksumOk(std::size_t position) noexcept;
    void emit(std::size_t position, SampleBlock &out);

    FrameFormat m_format;
    std::string m_error;
    std::size_t m_frameSize = 0;
    std::size_t m_counterSize = 0;
    std::size_t m_checksumSize = 0;
    ChecksumCalculator m_check;
    std::vector<std::uint8_t> m_pending;
    std::size_t m_read = 0; // consumed bytes at the front of m_pending (compacted lazily: linear time)
    bool m_locked = false;
    bool m_haveCounter = false;
    std::uint32_t m_lastCounter = 0;
    bool m_pendingDiscontinuity = false;
};
} // namespace qacq::io
