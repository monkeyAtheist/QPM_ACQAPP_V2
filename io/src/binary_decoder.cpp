#include "qacq/io/binary_decoder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace qacq::io
{
std::string_view sampleTypeName(SampleType type) noexcept
{
    switch (type)
    {
        case SampleType::Int8: return "int8";
        case SampleType::UInt8: return "uint8";
        case SampleType::Int16: return "int16";
        case SampleType::UInt16: return "uint16";
        case SampleType::Int32: return "int32";
        case SampleType::UInt32: return "uint32";
        case SampleType::Float32: return "float32";
        case SampleType::Float64: return "float64";
    }
    return "?";
}

std::size_t sampleSize(SampleType type) noexcept
{
    switch (type)
    {
        case SampleType::Int8:
        case SampleType::UInt8: return 1;
        case SampleType::Int16:
        case SampleType::UInt16: return 2;
        case SampleType::Int32:
        case SampleType::UInt32:
        case SampleType::Float32: return 4;
        case SampleType::Float64: return 8;
    }
    return 1;
}

namespace
{
std::uint64_t readUnsigned(const std::uint8_t *bytes, std::size_t size, ByteOrder order) noexcept
{
    std::uint64_t v = 0;
    if (order == ByteOrder::LittleEndian)
        for (std::size_t i = size; i-- > 0;) v = (v << 8) | bytes[i];
    else
        for (std::size_t i = 0; i < size; ++i) v = (v << 8) | bytes[i];
    return v;
}

void writeUnsigned(std::uint64_t v, std::size_t size, ByteOrder order, std::uint8_t *bytes) noexcept
{
    for (std::size_t i = 0; i < size; ++i)
    {
        const auto byte = static_cast<std::uint8_t>(v >> (8 * i));
        bytes[order == ByteOrder::LittleEndian ? i : size - 1 - i] = byte;
    }
}

template <typename T>
T saturate(double raw) noexcept
{
    if (std::isnan(raw)) return T{0};
    const double r = std::round(raw);
    if (r <= static_cast<double>(std::numeric_limits<T>::lowest())) return std::numeric_limits<T>::lowest();
    if (r >= static_cast<double>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    return static_cast<T>(r);
}
} // namespace

double readSample(const std::uint8_t *bytes, const SampleFormat &format) noexcept
{
    const std::size_t size = sampleSize(format.type);
    const std::uint64_t u = readUnsigned(bytes, size, format.order);
    double raw = 0.0;
    switch (format.type)
    {
        case SampleType::Int8: raw = static_cast<std::int8_t>(static_cast<std::uint8_t>(u)); break;
        case SampleType::UInt8: raw = static_cast<double>(u); break;
        case SampleType::Int16: raw = static_cast<std::int16_t>(static_cast<std::uint16_t>(u)); break;
        case SampleType::UInt16: raw = static_cast<double>(u); break;
        case SampleType::Int32: raw = static_cast<std::int32_t>(static_cast<std::uint32_t>(u)); break;
        case SampleType::UInt32: raw = static_cast<double>(u); break;
        case SampleType::Float32: raw = static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(u))); break;
        case SampleType::Float64: raw = std::bit_cast<double>(u); break;
    }
    return raw * format.scale + format.offset;
}

void writeSample(double value, const SampleFormat &format, std::uint8_t *bytes) noexcept
{
    const double raw = format.scale != 0.0 ? (value - format.offset) / format.scale : 0.0;
    std::uint64_t u = 0;
    switch (format.type)
    {
        case SampleType::Int8: u = static_cast<std::uint8_t>(saturate<std::int8_t>(raw)); break;
        case SampleType::UInt8: u = saturate<std::uint8_t>(raw); break;
        case SampleType::Int16: u = static_cast<std::uint16_t>(saturate<std::int16_t>(raw)); break;
        case SampleType::UInt16: u = saturate<std::uint16_t>(raw); break;
        case SampleType::Int32: u = static_cast<std::uint32_t>(saturate<std::int32_t>(raw)); break;
        case SampleType::UInt32: u = saturate<std::uint32_t>(raw); break;
        case SampleType::Float32: u = std::bit_cast<std::uint32_t>(static_cast<float>(raw)); break;
        case SampleType::Float64: u = std::bit_cast<std::uint64_t>(raw); break;
    }
    writeUnsigned(u, sampleSize(format.type), format.order, bytes);
}

// ------------------------------------------------------------------------------------------- raw stream
RawBinaryDecoder::RawBinaryDecoder(const SampleFormat &format, std::size_t channels)
    : m_format(format), m_channels(std::max<std::size_t>(1, channels))
{
}

void RawBinaryDecoder::decode(std::span<const std::uint8_t> bytes, SampleBlock &out)
{
    m_statistics.bytes += bytes.size();
    const std::size_t size = sampleSize(m_format.type);
    const std::size_t frame = size * m_channels;
    if (out.channelCount != m_channels)
    {
        out.clear();
        out.channelCount = m_channels;
    }
    auto emit = [&](const std::uint8_t *p) {
        for (std::size_t c = 0; c < m_channels; ++c) out.values.push_back(readSample(p + c * size, m_format));
        ++m_statistics.frames;
    };
    std::size_t i = 0;
    if (!m_pending.empty())
    {
        const std::size_t take = std::min(frame - m_pending.size(), bytes.size());
        for (; i < take; ++i) m_pending.push_back(bytes[i]); // a few bytes: completes the cut frame
        if (m_pending.size() < frame) return;
        emit(m_pending.data());
        m_pending.clear();
    }
    out.values.reserve(out.values.size() + (bytes.size() - i) / size);
    for (; i + frame <= bytes.size(); i += frame) emit(bytes.data() + i);
    m_pending.assign(bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.end());
}

// ------------------------------------------------------------------------------------------- frames
std::size_t frameSize(const FrameFormat &format) noexcept
{
    const std::size_t counter = format.counter == CounterField::None ? 0 : format.counter == CounterField::UInt8 ? 1 : 2;
    return format.sync.size() + counter + format.channels * format.samplesPerChannel * sampleSize(format.sample.type) +
           checksumSize(format.checksum);
}

std::string validateFrameFormat(const FrameFormat &format)
{
    if (format.sync.empty() || format.sync.size() > 8)
        return "Le motif de synchronisation doit compter de 1 à 8 octets.";
    if (format.channels == 0 || format.channels > 256) return "Le nombre de voies doit être compris entre 1 et 256.";
    if (format.samplesPerChannel == 0) return "Il faut au moins un échantillon par voie et par trame.";
    if (frameSize(format) > 65536) return "Une trame ne peut dépasser 65 536 octets.";
    if (!(std::isfinite(format.sample.scale) && format.sample.scale != 0.0))
        return "Le facteur d'échelle doit être fini et non nul.";
    return {};
}

std::vector<std::uint8_t> encodeFrame(const FrameFormat &format, std::uint32_t counter, std::span<const double> values)
{
    std::vector<std::uint8_t> frame(frameSize(format), 0);
    std::size_t p = 0;
    for (std::uint8_t b : format.sync) frame[p++] = b;
    const std::size_t counterSize = format.counter == CounterField::None ? 0 : format.counter == CounterField::UInt8 ? 1 : 2;
    writeUnsigned(counter, counterSize, format.sample.order, frame.data() + p);
    p += counterSize;
    const std::size_t count = format.channels * format.samplesPerChannel;
    const std::size_t size = sampleSize(format.sample.type);
    for (std::size_t i = 0; i < count; ++i, p += size)
        writeSample(i < values.size() ? values[i] : format.sample.offset, format.sample, frame.data() + p);
    const std::size_t from = format.checksumCoversSync ? 0 : format.sync.size();
    const std::uint32_t check =
        computeChecksum(format.checksum, std::span<const std::uint8_t>(frame.data() + from, p - from));
    writeUnsigned(check, checksumSize(format.checksum), format.sample.order, frame.data() + p);
    return frame;
}

FramedBinaryDecoder::FramedBinaryDecoder(const FrameFormat &format)
    : m_format(format), m_error(validateFrameFormat(format)), m_check(format.checksum)
{
    m_frameSize = frameSize(format);
    m_counterSize = format.counter == CounterField::None ? 0 : format.counter == CounterField::UInt8 ? 1 : 2;
    m_checksumSize = checksumSize(format.checksum);
}

void FramedBinaryDecoder::reset()
{
    m_pending.clear();
    m_read = 0;
    m_locked = false;
    m_haveCounter = false;
    m_pendingDiscontinuity = false;
}

bool FramedBinaryDecoder::syncAt(std::size_t position) const noexcept
{
    return std::equal(m_format.sync.begin(), m_format.sync.end(),
                      m_pending.begin() + static_cast<std::ptrdiff_t>(position));
}

bool FramedBinaryDecoder::checksumOk(std::size_t position) noexcept
{
    if (m_checksumSize == 0) return true;
    const std::size_t from = position + (m_format.checksumCoversSync ? 0 : m_format.sync.size());
    const std::size_t to = position + m_frameSize - m_checksumSize;
    const std::uint32_t computed = m_check.compute(std::span<const std::uint8_t>(m_pending.data() + from, to - from));
    const auto stored =
        static_cast<std::uint32_t>(readUnsigned(m_pending.data() + to, m_checksumSize, m_format.sample.order));
    return computed == stored;
}

void FramedBinaryDecoder::emit(std::size_t position, SampleBlock &out)
{
    const std::uint8_t *p = m_pending.data() + position + m_format.sync.size();
    if (m_counterSize)
    {
        const auto counter = static_cast<std::uint32_t>(readUnsigned(p, m_counterSize, m_format.sample.order));
        if (m_haveCounter)
        {
            const std::uint32_t modulus = m_counterSize == 1 ? 0x100u : 0x10000u;
            const std::uint32_t expected = (m_lastCounter + 1) % modulus;
            if (counter != expected)
            {
                m_statistics.lostFrames += (counter + modulus - expected) % modulus;
                m_pendingDiscontinuity = true;
            }
        }
        m_lastCounter = counter;
        m_haveCounter = true;
        p += m_counterSize;
    }
    if (out.channelCount != m_format.channels)
    {
        out.clear();
        out.channelCount = m_format.channels;
    }
    if (m_pendingDiscontinuity)
    {
        out.discontinuities.push_back(out.frameCount());
        m_pendingDiscontinuity = false;
    }
    const std::size_t size = sampleSize(m_format.sample.type);
    const std::size_t count = m_format.channels * m_format.samplesPerChannel;
    for (std::size_t i = 0; i < count; ++i) out.values.push_back(readSample(p + i * size, m_format.sample));
    m_statistics.frames += m_format.samplesPerChannel;
}

void FramedBinaryDecoder::decode(std::span<const std::uint8_t> bytes, SampleBlock &out)
{
    m_statistics.bytes += bytes.size();
    if (!m_error.empty())
    {
        m_statistics.skippedBytes += bytes.size();
        return;
    }
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
    const bool confirm = m_checksumSize == 0;
    auto loseSync = [&] {
        if (m_locked)
        {
            ++m_statistics.rejected;
            m_locked = false;
        }
        // With a counter, the counter alone tells whether frames are missing (garbage between two good
        // frames loses nothing); without one, any loss of synchronisation may hide lost frames.
        if (m_statistics.frames > 0 && m_counterSize == 0) m_pendingDiscontinuity = true;
    };
    while (m_pending.size() - m_read >= m_frameSize)
    {
        if (!syncAt(m_read))
        {
            loseSync();
            const auto from = m_pending.begin() + static_cast<std::ptrdiff_t>(m_read + 1);
            const auto next = std::find(from, m_pending.end(), m_format.sync.front());
            const auto skip = static_cast<std::size_t>(next - from) + 1;
            m_statistics.skippedBytes += skip;
            m_read += skip;
            continue;
        }
        if (!checksumOk(m_read))
        {
            ++m_statistics.checksumErrors;
            loseSync();
            ++m_statistics.skippedBytes;
            ++m_read;
            continue;
        }
        if (confirm && !m_locked)
        {
            if (m_pending.size() - m_read < 2 * m_frameSize) break; // wait for the next frame
            if (!syncAt(m_read + m_frameSize))
            {
                ++m_statistics.skippedBytes;
                ++m_read;
                continue;
            }
        }
        m_locked = true;
        emit(m_read, out);
        m_read += m_frameSize;
    }
    if (m_read == m_pending.size())
    {
        m_pending.clear();
        m_read = 0;
    }
    else if (m_read > 0 && 2 * m_read >= m_pending.size())
    {
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(m_read));
        m_read = 0;
    }
}
} // namespace qacq::io
