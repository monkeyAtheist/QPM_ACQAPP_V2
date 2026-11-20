#pragma once

// The acquisition in progress.
//
// SamplePipe carries decoded frames from the acquisition thread (producer) to the interface thread
// (consumer) without a lock: values and discontinuities go through two SpscRing. When the interface falls
// behind and the ring is full, whole frames are dropped, counted, and marked as a discontinuity.
//
// LiveRecorder keeps what the consumer received, channel by channel, in blocks of 64 Ki samples (no
// reallocation of the whole record as it grows). Past its memory limit it either stops recording (and
// says so) or drops its oldest blocks (sliding window: the last minutes of a long session). Frames are
// numbered from the start of the acquisition, dropped ones included.

#include "qacq/io/decoder.h"
#include "qacq/io/spsc_ring.h"
#include "qacq/model/signal.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace qacq::acquisition
{
class SamplePipe
{
public:
    // capacityFrames: frames the ring can hold (rounded up to a power of two of values).
    SamplePipe(std::size_t channels, std::size_t capacityFrames);

    std::size_t channelCount() const noexcept { return m_channels; }

    // Producer. Pushes the whole frames of the block (its channel count must match); returns the frames
    // pushed (the others are dropped and counted).
    std::size_t push(const io::SampleBlock &block);
    std::uint64_t droppedFrames() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

    // Consumer. Appends the frames available (interleaved) to out, and the absolute frame indices before
    // which a discontinuity happened to breaks. Returns the frames read.
    std::size_t pull(std::vector<double> &out, std::vector<std::uint64_t> &breaks, std::size_t maximumFrames = SIZE_MAX);

private:
    std::size_t m_channels;
    io::SpscRing<double> m_values;
    io::SpscRing<std::uint64_t> m_breaks;
    // Producer side.
    std::uint64_t m_produced = 0;     // frames pushed (dropped ones are not numbered)
    bool m_breakPending = false;      // a drop happened: mark the next frame pushed
    std::atomic<std::uint64_t> m_dropped{0};
    // Consumer side.
    std::uint64_t m_consumed = 0;
    std::vector<std::uint64_t> m_breakBuffer;
};

struct RecorderLimits
{
    std::size_t maximumBytes = std::size_t{512} << 20; // 512 MiB of samples
    double maximumSeconds = 0.0;                      // 0: no limit (with a known rate)
    bool keepLatest = false;                          // false: stop at the limit; true: sliding window
};

class LiveRecorder
{
public:
    static constexpr std::size_t kBlockSamples = std::size_t{1} << 16;

    explicit LiveRecorder(std::size_t channels = 1, const RecorderLimits &limits = {}, double sampleRateHz = 0.0);

    void reset(std::size_t channels);
    void setLimits(const RecorderLimits &limits, double sampleRateHz);
    std::size_t channelCount() const noexcept { return m_channels.size(); }

    // Appends interleaved frames; breaks are absolute frame indices (from the pipe). Returns the frames
    // kept (0 once full in stop mode).
    std::size_t append(std::span<const double> interleaved, std::span<const std::uint64_t> breaks = {});
    // Marks a discontinuity before the next frame (source reconnected, resynchronised...).
    void markDiscontinuity();

    std::uint64_t totalFrames() const noexcept { return m_total; }      // received since the start
    std::uint64_t firstFrame() const noexcept { return m_first; }       // number of the oldest frame kept
    std::size_t storedFrames() const noexcept { return static_cast<std::size_t>(m_total - m_first); }
    std::size_t byteSize() const noexcept { return storedFrames() * channelCount() * sizeof(double); }
    bool full() const noexcept { return m_full; }                        // stop mode: limit reached
    std::uint64_t refusedFrames() const noexcept { return m_refused; }  // not kept because full

    // Copies frames [first, first + count) of a channel (clipped to what is kept); returns the first
    // frame actually copied.
    std::uint64_t copy(std::size_t channel, std::uint64_t first, std::size_t count, std::vector<double> &out) const;
    void copyLatest(std::size_t channel, std::size_t count, std::vector<double> &out) const;
    double value(std::size_t channel, std::uint64_t frame) const; // NaN when not kept
    // Discontinuities among the frames kept (absolute numbers).
    std::vector<std::uint64_t> discontinuities() const;

    // Everything kept, as signal data (discontinuities relative to the first frame kept).
    std::shared_ptr<model::SampleData> snapshot() const;

private:
    std::size_t limitFrames() const noexcept;
    void dropOldestBlock();

    std::vector<std::deque<std::vector<double>>> m_channels; // blocks of kBlockSamples
    std::deque<std::uint64_t> m_breaks;
    RecorderLimits m_limits;
    double m_rate = 0.0;
    std::uint64_t m_total = 0;
    std::uint64_t m_first = 0;
    std::uint64_t m_refused = 0;
    bool m_full = false;
    bool m_breakPending = false;
};
} // namespace qacq::acquisition
