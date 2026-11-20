#pragma once

// Continuous recording to disk while acquiring, for records longer than memory: frames are appended as
// they come, in a format the import reads back.
// - CSV: a header line, one line per frame (time column optional), flushed every second or so.
// - WAV: float 32 bits (no clipping) or PCM 16 bits; the sizes in the header are written at the end, and
//   a file left unfinished (crash, power cut) still opens: the reader takes the data up to the end of file.

#include "qacq/model/signal.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace qacq::acquisition
{
enum class StreamFormat
{
    Csv,
    Wav
};

struct StreamOptions
{
    StreamFormat format = StreamFormat::Csv;
    bool decimalComma = false; // CSV
    bool timeColumn = true;    // CSV
    bool wavFloat = true;      // WAV: float 32 bits, else PCM 16 bits
    double fullScale = 1.0;    // WAV PCM: value mapped to the largest code
};

class StreamWriter
{
public:
    StreamWriter() = default;
    ~StreamWriter();
    StreamWriter(const StreamWriter &) = delete;
    StreamWriter &operator=(const StreamWriter &) = delete;

    // Empty on success, else the reason (in French).
    std::string open(const std::filesystem::path &file, const std::vector<model::ChannelInfo> &channels,
                     double sampleRateHz, const StreamOptions &options = {});
    std::string append(std::span<const double> interleaved);
    std::string close(); // writes the final sizes (WAV) and closes; safe to call twice

    bool isOpen() const noexcept { return m_out.is_open(); }
    std::uint64_t framesWritten() const noexcept { return m_frames; }
    std::uint64_t bytesWritten() const noexcept { return m_bytes; }
    std::uint64_t clippedSamples() const noexcept { return m_clipped; }

private:
    std::ofstream m_out;
    StreamOptions m_options;
    std::size_t m_channels = 0;
    double m_rate = 1.0;
    std::uint64_t m_frames = 0;
    std::uint64_t m_bytes = 0;
    std::uint64_t m_clipped = 0;
    std::uint64_t m_dataStart = 0;
    std::uint64_t m_sinceFlush = 0;
    std::string m_line;
};
} // namespace qacq::acquisition
