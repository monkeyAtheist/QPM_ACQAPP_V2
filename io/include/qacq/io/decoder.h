#pragma once

// Streaming decoders: bytes as they arrive from a serial port, a socket or an instrument (cut anywhere,
// even in the middle of a number or of a frame) -> frames of simultaneous samples, one per channel.
// Every decoder works in time linear in the bytes received, whatever the size of the blocks, and keeps
// a bounded amount of pending data.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qacq::io
{
// Decoded frames, interleaved: values[f * channelCount + c].
struct SampleBlock
{
    std::size_t channelCount = 0;
    std::vector<double> values;
    // Frame indices (in this block) before which data is missing or the stream restarts: lost frames
    // (counter jump), rejected data, a new instrument record. Displays break the trace there.
    std::vector<std::size_t> discontinuities;
    // Channel names when the stream gives them (CSV header, "label:value" pairs). Empty otherwise.
    std::vector<std::string> channelNames;

    std::size_t frameCount() const noexcept { return channelCount ? values.size() / channelCount : 0; }
    void clear()
    {
        values.clear();
        discontinuities.clear();
    }
};

struct DecoderStatistics
{
    std::uint64_t bytes = 0;            // bytes received
    std::uint64_t frames = 0;           // frames produced
    std::uint64_t rejected = 0;         // lines or frames dropped (malformed, wrong column count, bad checksum)
    std::uint64_t checksumErrors = 0;   // binary frames with a sync pattern but a wrong checksum
    std::uint64_t overflows = 0;        // lines longer than the limit
    std::uint64_t skippedBytes = 0;     // bytes discarded while looking for a frame start
    std::uint64_t lostFrames = 0;       // frames missing according to the frame counter
    std::uint64_t comments = 0;         // ignored text lines ("# ...", headers)
};

class Decoder
{
public:
    virtual ~Decoder() = default;

    // Appends the frames completed by these bytes to out (out.channelCount is set by the decoder).
    virtual void decode(std::span<const std::uint8_t> bytes, SampleBlock &out) = 0;
    // Forgets pending bytes and the stream state (channel count found, frame counter), keeps the settings.
    virtual void reset() = 0;
    virtual std::size_t channelCount() const noexcept = 0; // 0 while unknown
    virtual std::size_t pendingBytes() const noexcept = 0;

    const DecoderStatistics &statistics() const noexcept { return m_statistics; }
    void resetStatistics() noexcept { m_statistics = {}; }

protected:
    DecoderStatistics m_statistics;
};
} // namespace qacq::io
