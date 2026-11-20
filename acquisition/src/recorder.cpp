#include "qacq/acquisition/recorder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qacq::acquisition
{
// ------------------------------------------------------------------------------------------- pipe
SamplePipe::SamplePipe(std::size_t channels, std::size_t capacityFrames)
    : m_channels(std::max<std::size_t>(1, channels)),
      m_values(std::max<std::size_t>(2, capacityFrames) * std::max<std::size_t>(1, channels)), m_breaks(4096)
{
}

std::size_t SamplePipe::push(const io::SampleBlock &block)
{
    if (block.channelCount != m_channels || block.values.empty()) return 0;
    const std::size_t frames = block.frameCount();
    // Free room seen from the producer: never more than the truth (the consumer may have read since).
    const std::size_t room = (m_values.capacity() - m_values.size()) / m_channels;
    const std::size_t pushed = std::min(frames, room);
    if (pushed > 0)
        m_values.pushAll(std::span<const double>(block.values.data(), pushed * m_channels));
    // Discontinuities after the values, numbered among the frames that went through.
    if (m_breakPending && pushed > 0)
    {
        const std::uint64_t at = m_produced;
        m_breaks.push(std::span<const std::uint64_t>(&at, 1));
        m_breakPending = false;
    }
    for (std::size_t index : block.discontinuities)
    {
        if (index >= pushed) break;
        const std::uint64_t at = m_produced + index;
        m_breaks.push(std::span<const std::uint64_t>(&at, 1));
    }
    if (pushed < frames)
    {
        m_dropped.fetch_add(frames - pushed, std::memory_order_relaxed);
        m_breakPending = true;
    }
    m_produced += pushed;
    return pushed;
}

std::size_t SamplePipe::pull(std::vector<double> &out, std::vector<std::uint64_t> &breaks, std::size_t maximumFrames)
{
    // Breaks first: the producer publishes them after their values, so every break read here refers to
    // a frame already readable. Breaks beyond what this call reads wait for the next one.
    std::uint64_t chunk[256];
    for (;;)
    {
        const std::size_t got = m_breaks.pop(std::span<std::uint64_t>(chunk, 256));
        m_breakBuffer.insert(m_breakBuffer.end(), chunk, chunk + got);
        if (got < 256) break;
    }
    const std::size_t available = m_values.size() / m_channels;
    const std::size_t frames = std::min(available, maximumFrames);
    std::size_t read = 0;
    if (frames > 0)
    {
        const std::size_t start = out.size();
        out.resize(start + frames * m_channels);
        const std::size_t got = m_values.pop(std::span<double>(out.data() + start, frames * m_channels));
        out.resize(start + got);
        read = got / m_channels;
    }
    m_consumed += read;
    auto keep = std::stable_partition(m_breakBuffer.begin(), m_breakBuffer.end(), [&](std::uint64_t b) { return b < m_consumed; });
    breaks.insert(breaks.end(), m_breakBuffer.begin(), keep);
    m_breakBuffer.erase(m_breakBuffer.begin(), keep);
    return read;
}

// ------------------------------------------------------------------------------------------- recorder
LiveRecorder::LiveRecorder(std::size_t channels, const RecorderLimits &limits, double sampleRateHz)
    : m_limits(limits), m_rate(sampleRateHz)
{
    reset(channels);
}

void LiveRecorder::reset(std::size_t channels)
{
    m_channels.assign(std::max<std::size_t>(1, channels), {});
    m_breaks.clear();
    m_total = 0;
    m_first = 0;
    m_refused = 0;
    m_full = false;
    m_breakPending = false;
}

void LiveRecorder::setLimits(const RecorderLimits &limits, double sampleRateHz)
{
    m_limits = limits;
    m_rate = sampleRateHz;
}

std::size_t LiveRecorder::limitFrames() const noexcept
{
    std::size_t frames = m_limits.maximumBytes / (sizeof(double) * channelCount());
    if (m_limits.maximumSeconds > 0.0 && m_rate > 0.0)
    {
        const double bySeconds = std::ceil(m_limits.maximumSeconds * m_rate);
        if (bySeconds < static_cast<double>(frames)) frames = static_cast<std::size_t>(bySeconds);
    }
    return std::max<std::size_t>(1, frames);
}

void LiveRecorder::markDiscontinuity()
{
    m_breakPending = true;
}

std::size_t LiveRecorder::append(std::span<const double> interleaved, std::span<const std::uint64_t> breaks)
{
    const std::size_t channels = channelCount();
    std::size_t frames = interleaved.size() / channels;
    if (frames == 0) return 0;
    std::size_t accepted = frames;
    if (!m_limits.keepLatest)
    {
        const std::size_t room = m_full ? 0 : limitFrames() - std::min(limitFrames(), storedFrames());
        accepted = std::min(frames, room);
        if (accepted < frames)
        {
            m_full = true;
            m_refused += frames - accepted;
        }
    }
    const std::uint64_t start = m_total;
    if (m_breakPending && accepted > 0)
    {
        if (start > m_first) m_breaks.push_back(start);
        m_breakPending = false;
    }
    for (std::uint64_t b : breaks)
        if (b >= start && b < start + accepted && b > m_first && (m_breaks.empty() || m_breaks.back() < b)) m_breaks.push_back(b);

    for (std::size_t c = 0; c < channels; ++c)
    {
        auto &blocks = m_channels[c];
        std::size_t f = 0;
        while (f < accepted)
        {
            if (blocks.empty() || blocks.back().size() == kBlockSamples)
            {
                blocks.emplace_back();
                blocks.back().reserve(kBlockSamples);
            }
            auto &block = blocks.back();
            const std::size_t take = std::min(accepted - f, kBlockSamples - block.size());
            for (std::size_t k = 0; k < take; ++k) block.push_back(interleaved[(f + k) * channels + c]);
            f += take;
        }
    }
    m_total += accepted;
    if (m_limits.keepLatest)
        while (storedFrames() >= limitFrames() + kBlockSamples) dropOldestBlock();
    return accepted;
}

void LiveRecorder::dropOldestBlock()
{
    for (auto &blocks : m_channels)
        if (!blocks.empty()) blocks.pop_front();
    m_first += kBlockSamples;
    while (!m_breaks.empty() && m_breaks.front() <= m_first) m_breaks.pop_front();
}

std::uint64_t LiveRecorder::copy(std::size_t channel, std::uint64_t first, std::size_t count, std::vector<double> &out) const
{
    out.clear();
    if (channel >= channelCount()) return first;
    const std::uint64_t from = std::max(first, m_first);
    const std::uint64_t to = std::min<std::uint64_t>(first + count, m_total);
    if (from >= to) return from;
    out.reserve(static_cast<std::size_t>(to - from));
    const auto &blocks = m_channels[channel];
    std::uint64_t frame = from;
    while (frame < to)
    {
        const std::uint64_t offset = frame - m_first;
        const auto &block = blocks[static_cast<std::size_t>(offset / kBlockSamples)];
        const std::size_t inBlock = static_cast<std::size_t>(offset % kBlockSamples);
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(to - frame, block.size() - inBlock));
        out.insert(out.end(), block.begin() + static_cast<std::ptrdiff_t>(inBlock),
                   block.begin() + static_cast<std::ptrdiff_t>(inBlock + take));
        frame += take;
    }
    return from;
}

void LiveRecorder::copyLatest(std::size_t channel, std::size_t count, std::vector<double> &out) const
{
    const std::uint64_t first = m_total > count ? m_total - count : 0;
    copy(channel, first, count, out);
}

double LiveRecorder::value(std::size_t channel, std::uint64_t frame) const
{
    if (channel >= channelCount() || frame < m_first || frame >= m_total) return std::numeric_limits<double>::quiet_NaN();
    const std::uint64_t offset = frame - m_first;
    return m_channels[channel][static_cast<std::size_t>(offset / kBlockSamples)][static_cast<std::size_t>(offset % kBlockSamples)];
}

std::vector<std::uint64_t> LiveRecorder::discontinuities() const
{
    return {m_breaks.begin(), m_breaks.end()};
}

std::shared_ptr<model::SampleData> LiveRecorder::snapshot() const
{
    auto data = std::make_shared<model::SampleData>();
    data->channels.resize(channelCount());
    for (std::size_t c = 0; c < channelCount(); ++c) copy(c, m_first, storedFrames(), data->channels[c]);
    for (std::uint64_t b : m_breaks)
        if (b > m_first && b < m_total) data->discontinuities.push_back(static_cast<std::size_t>(b - m_first));
    return data;
}
} // namespace qacq::acquisition
