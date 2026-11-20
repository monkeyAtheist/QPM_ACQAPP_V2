#pragma once

// Text lines of numbers, as a microcontroller prints them (printf, Serial.println) or as an instrument
// answers a query: one line = one frame, columns = channels.
//
//   "1.25,3.5\r\n"           comma, semicolon, tab or spaces between columns (runs count as one)
//   "1,25;3,5"               decimal comma (option): then columns split by ';', tab or spaces
//   "temp:21.5 hum:40"       "label:value" or "label=value" pairs (Arduino Serial Plotter): the labels of
//                            the first such line name the channels
//   "t,ch1,ch2"              a line without any number before the first data line: the channel names
//   "# comment"              ignored
// Numbers: C locale, optional sign, exponent, "nan" and "inf" (std::from_chars). Lines end with '\n',
// '\r' or both. A line whose column count differs from the channel count, or with a token that is not a
// number, is rejected and counted (and marks a discontinuity).

#include "qacq/io/decoder.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::io
{
struct TextFormat
{
    std::size_t channelCount = 0;        // 0: set by the first data line
    bool decimalComma = false;
    std::size_t maximumLineLength = 4096; // longer lines are dropped (bounded memory on a stream without newlines)
};

// Parses one line (no terminator). Returns the values, or nothing if a token is not a number.
// Labels found ("label:value") are stored in labels when it is not null.
std::optional<std::vector<double>> parseNumberLine(std::string_view line, bool decimalComma,
                                                   std::vector<std::string> *labels = nullptr);

class TextDecoder final : public Decoder
{
public:
    explicit TextDecoder(const TextFormat &format = {});

    void decode(std::span<const std::uint8_t> bytes, SampleBlock &out) override;
    void reset() override;
    std::size_t channelCount() const noexcept override { return m_channels; }
    std::size_t pendingBytes() const noexcept override { return m_carry.size(); }

    const TextFormat &format() const noexcept { return m_format; }
    void setFormat(const TextFormat &format); // resets the stream
    const std::vector<std::string> &channelNames() const noexcept { return m_names; }

private:
    void line(std::string_view text, SampleBlock &out);

    TextFormat m_format;
    std::string m_carry;      // start of a line cut by the end of the previous block
    bool m_overflow = false;  // dropping the rest of an over-long line
    std::size_t m_channels = 0;
    std::vector<std::string> m_names;
    bool m_pendingDiscontinuity = false;
    std::vector<double> m_values;
    std::vector<std::string> m_labels;
    std::vector<std::string> m_words;
};
} // namespace qacq::io
