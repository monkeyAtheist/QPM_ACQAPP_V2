#pragma once

// Answers of SCPI instruments (oscilloscopes, multimeters, generators) to queries such as
// MEAS:VOLT:DC? or :WAVeform:DATA?.
//
// An answer is either a line of ASCII numbers ("+1.234E-03,+1.240E-03\n") or an IEEE 488.2 block:
//   "#<n><length on n digits><length bytes>" (definite length, usually followed by '\n'), or
//   "#0<bytes>\n" (indefinite length: up to the newline; only safe for ASCII-like data).
// Values are interleaved by channel: with 1 channel every value is a sample (a waveform); with 3 channels
// "MEAS:VOLT? (@1,2,3)" gives one frame. An answer that holds two frames or more is a record of its own
// (a waveform): a discontinuity marks its start. Single-frame answers (a polled measurement) follow each
// other without a break. SCPI-99 special values: 9.91E37 -> NaN, +-9.9E37 -> +-infinity.

#include "qacq/io/binary_decoder.h"
#include "qacq/io/decoder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::io
{
struct BlockHeader
{
    enum class Status
    {
        Incomplete, // more bytes needed
        Valid,
        Invalid
    };
    Status status = Status::Incomplete;
    std::size_t headerSize = 0; // bytes before the data
    std::size_t dataSize = 0;   // definite blocks
    bool indefinite = false;
};

// Parses a block header at the start of bytes (which must start with '#').
BlockHeader parseBlockHeader(std::span<const std::uint8_t> bytes, std::size_t maximumDataSize) noexcept;

// "#<n><length>" for a definite block of dataSize bytes (to build test answers and simulated instruments).
std::string blockHeader(std::size_t dataSize);

struct ScpiError
{
    int code = 0;
    std::string message;
};
// Parses an answer of SYSTem:ERRor? such as -113,"Undefined header" or +0,"No error".
std::optional<ScpiError> parseScpiError(std::string_view answer);

struct ScpiFormat
{
    std::size_t channels = 1;
    SampleFormat block{SampleType::Int8, ByteOrder::LittleEndian, 1.0, 0.0}; // binary block samples
    std::size_t maximumAnswerBytes = 64u * 1024u * 1024u;
};

class ScpiDecoder final : public Decoder
{
public:
    explicit ScpiDecoder(const ScpiFormat &format = {});

    void decode(std::span<const std::uint8_t> bytes, SampleBlock &out) override;
    void reset() override;
    std::size_t channelCount() const noexcept override { return m_format.channels; }
    std::size_t pendingBytes() const noexcept override { return m_line.size() + m_header.size() + m_sample.size(); }

    // Answers begun (a text answer counts once complete, a block from its header on).
    std::uint64_t answers() const noexcept { return m_answers; }
    // No answer in progress: the last one is complete (a block wholly received).
    bool betweenAnswers() const noexcept { return m_state == State::Idle; }

private:
    enum class State
    {
        Idle,
        Header,
        Block,
        Indefinite,
        Line
    };
    void finishLine(SampleBlock &out);
    void beginRecord(std::size_t frames, SampleBlock &out);
    void blockByte(std::uint8_t byte, SampleBlock &out);
    void prepare(SampleBlock &out);

    ScpiFormat m_format;
    State m_state = State::Idle;
    std::string m_line;
    std::vector<std::uint8_t> m_header;
    std::vector<std::uint8_t> m_sample; // bytes of the frame being assembled inside a block
    std::size_t m_remaining = 0;
    std::uint64_t m_answers = 0;
    bool m_rejectedTail = false;
    bool m_lastWasRecord = false;
    bool m_dropping = false; // over-long text answer, dropped up to its newline
};
} // namespace qacq::io
