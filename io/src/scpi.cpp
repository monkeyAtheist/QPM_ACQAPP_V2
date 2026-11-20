#include "qacq/io/scpi.h"

#include "qacq/io/text_decoder.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace qacq::io
{
BlockHeader parseBlockHeader(std::span<const std::uint8_t> bytes, std::size_t maximumDataSize) noexcept
{
    BlockHeader h;
    if (bytes.empty()) return h;
    if (bytes[0] != '#')
    {
        h.status = BlockHeader::Status::Invalid;
        return h;
    }
    if (bytes.size() < 2) return h;
    const std::uint8_t d = bytes[1];
    if (d < '0' || d > '9')
    {
        h.status = BlockHeader::Status::Invalid;
        return h;
    }
    const std::size_t digits = static_cast<std::size_t>(d - '0');
    if (digits == 0)
    {
        h.status = BlockHeader::Status::Valid;
        h.indefinite = true;
        h.headerSize = 2;
        return h;
    }
    if (bytes.size() < 2 + digits) return h;
    std::size_t size = 0;
    for (std::size_t i = 0; i < digits; ++i)
    {
        const std::uint8_t c = bytes[2 + i];
        if (c < '0' || c > '9')
        {
            h.status = BlockHeader::Status::Invalid;
            return h;
        }
        size = size * 10 + static_cast<std::size_t>(c - '0');
    }
    if (size > maximumDataSize)
    {
        h.status = BlockHeader::Status::Invalid;
        return h;
    }
    h.status = BlockHeader::Status::Valid;
    h.headerSize = 2 + digits;
    h.dataSize = size;
    return h;
}

std::string blockHeader(std::size_t dataSize)
{
    const std::string length = std::to_string(dataSize);
    return "#" + std::to_string(length.size()) + length;
}

std::optional<ScpiError> parseScpiError(std::string_view answer)
{
    while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r' || answer.back() == ' '))
        answer.remove_suffix(1);
    while (!answer.empty() && answer.front() == ' ') answer.remove_prefix(1);
    const std::size_t comma = answer.find(',');
    std::string_view number = answer.substr(0, comma);
    if (!number.empty() && number.front() == '+') number.remove_prefix(1);
    int code = 0;
    const auto r = std::from_chars(number.data(), number.data() + number.size(), code);
    if (r.ec != std::errc() || r.ptr != number.data() + number.size()) return std::nullopt;
    ScpiError e;
    e.code = code;
    if (comma != std::string_view::npos)
    {
        std::string_view text = answer.substr(comma + 1);
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
        e.message = std::string(text);
    }
    return e;
}

ScpiDecoder::ScpiDecoder(const ScpiFormat &format) : m_format(format)
{
    m_format.channels = std::max<std::size_t>(1, m_format.channels);
}

void ScpiDecoder::reset()
{
    m_state = State::Idle;
    m_line.clear();
    m_header.clear();
    m_sample.clear();
    m_remaining = 0;
    m_rejectedTail = false;
    m_lastWasRecord = false;
    m_dropping = false;
}

void ScpiDecoder::prepare(SampleBlock &out)
{
    if (out.channelCount != m_format.channels)
    {
        out.clear();
        out.channelCount = m_format.channels;
    }
}

void ScpiDecoder::beginRecord(std::size_t frames, SampleBlock &out)
{
    const bool record = frames >= 2;
    if ((record || m_lastWasRecord) && m_statistics.frames > 0) out.discontinuities.push_back(out.frameCount());
    m_lastWasRecord = record;
    ++m_answers;
}

void ScpiDecoder::finishLine(SampleBlock &out)
{
    std::string_view text = m_line;
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (text.empty()) return;
    auto values = parseNumberLine(text, false);
    if (!values)
    {
        ++m_statistics.comments; // a text answer (*IDN?, a status word): not samples
        ++m_answers;
        return;
    }
    if (values->empty()) return;
    const std::size_t channels = m_format.channels;
    if (values->size() % channels != 0)
    {
        ++m_statistics.rejected;
        ++m_answers;
        return;
    }
    for (double &v : *values)
    {
        // SCPI-99: 9.91E37 is "not a number", 9.9E37 is infinity.
        if (std::abs(v - 9.91e37) < 1e33) v = std::numeric_limits<double>::quiet_NaN();
        else if (std::abs(std::abs(v) - 9.9e37) < 1e33) v = std::copysign(std::numeric_limits<double>::infinity(), v);
    }
    const std::size_t frames = values->size() / channels;
    beginRecord(frames, out);
    out.values.insert(out.values.end(), values->begin(), values->end());
    m_statistics.frames += frames;
}

void ScpiDecoder::blockByte(std::uint8_t byte, SampleBlock &out)
{
    m_sample.push_back(byte);
    const std::size_t size = sampleSize(m_format.block.type);
    if (m_sample.size() == size * m_format.channels)
    {
        for (std::size_t c = 0; c < m_format.channels; ++c) out.values.push_back(readSample(m_sample.data() + c * size, m_format.block));
        ++m_statistics.frames;
        m_sample.clear();
    }
}

void ScpiDecoder::decode(std::span<const std::uint8_t> bytes, SampleBlock &out)
{
    m_statistics.bytes += bytes.size();
    prepare(out);
    const std::size_t n = bytes.size();
    const std::size_t sampleBytes = sampleSize(m_format.block.type);
    const std::size_t frameBytes = sampleBytes * m_format.channels;
    std::size_t i = 0;
    while (i < n)
    {
        switch (m_state)
        {
            case State::Idle:
            {
                const std::uint8_t c = bytes[i];
                if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == ';')
                {
                    ++i;
                }
                else if (c == '#')
                {
                    m_header.assign(1, c);
                    m_state = State::Header;
                    ++i;
                }
                else
                {
                    m_line.clear();
                    m_state = State::Line;
                }
                break;
            }
            case State::Header:
            {
                m_header.push_back(bytes[i++]);
                const BlockHeader h = parseBlockHeader(m_header, m_format.maximumAnswerBytes);
                if (h.status == BlockHeader::Status::Incomplete) break;
                if (h.status == BlockHeader::Status::Invalid)
                {
                    ++m_statistics.rejected;
                    m_statistics.skippedBytes += m_header.size();
                    m_state = State::Idle;
                    break;
                }
                m_sample.clear();
                if (h.indefinite)
                {
                    beginRecord(2, out);
                    m_state = State::Indefinite;
                    break;
                }
                m_remaining = h.dataSize;
                m_rejectedTail = h.dataSize % frameBytes != 0;
                beginRecord(h.dataSize / frameBytes, out);
                m_state = m_remaining ? State::Block : State::Idle;
                break;
            }
            case State::Block:
            {
                std::size_t take = std::min(m_remaining, n - i);
                m_remaining -= take;
                // Finish a frame cut by the previous block of bytes, then whole frames in place.
                while (take > 0 && !m_sample.empty())
                {
                    blockByte(bytes[i++], out);
                    --take;
                }
                const std::size_t whole = take / frameBytes * frameBytes;
                for (std::size_t k = 0; k < whole; k += frameBytes)
                {
                    for (std::size_t c = 0; c < m_format.channels; ++c)
                        out.values.push_back(readSample(bytes.data() + i + k + c * sampleBytes, m_format.block));
                }
                m_statistics.frames += whole / frameBytes;
                i += whole;
                take -= whole;
                for (; take > 0; --take) blockByte(bytes[i++], out);
                if (m_remaining == 0)
                {
                    if (m_rejectedTail || !m_sample.empty()) ++m_statistics.rejected;
                    m_sample.clear();
                    m_state = State::Idle;
                }
                break;
            }
            case State::Indefinite:
            {
                const std::uint8_t c = bytes[i++];
                if (c == '\n')
                {
                    if (!m_sample.empty()) ++m_statistics.rejected;
                    m_sample.clear();
                    m_state = State::Idle;
                }
                else
                {
                    blockByte(c, out);
                }
                break;
            }
            case State::Line:
            {
                std::size_t end = i;
                while (end < n && bytes[end] != '\n') ++end;
                const std::size_t piece = end - i;
                if (!m_dropping)
                {
                    if (m_line.size() + piece > m_format.maximumAnswerBytes)
                    {
                        ++m_statistics.overflows; // dropped up to the newline
                        m_line.clear();
                        m_dropping = true;
                    }
                    else
                    {
                        m_line.append(reinterpret_cast<const char *>(bytes.data() + i), piece);
                    }
                }
                i = end;
                if (end < n)
                {
                    if (m_dropping) ++m_statistics.rejected;
                    else finishLine(out);
                    m_dropping = false;
                    m_line.clear();
                    m_state = State::Idle;
                    ++i;
                }
                break;
            }
        }
    }
}
} // namespace qacq::io
