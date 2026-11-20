#include "qacq/io/text_decoder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace qacq::io
{
namespace
{
bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

bool isSeparator(char c, bool decimalComma) noexcept
{
    return isSpace(c) || c == ';' || (!decimalComma && c == ',');
}

bool parseNumber(std::string_view token, bool decimalComma, double &value) noexcept
{
    if (!token.empty() && token.front() == '+') token.remove_prefix(1);
    if (token.empty()) return false;
    const char *first = token.data();
    const char *last = token.data() + token.size();
    std::array<char, 64> buffer{};
    if (decimalComma && token.find(',') != std::string_view::npos)
    {
        if (token.size() > buffer.size()) return false;
        std::copy(token.begin(), token.end(), buffer.begin());
        std::replace(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(token.size()), ',', '.');
        first = buffer.data();
        last = buffer.data() + token.size();
    }
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    return result.ec == std::errc() && result.ptr == last;
}

// Splits the line into tokens and parses them. Returns false if a token is not a number. nonNumeric
// counts the tokens that are not numbers (to recognise a header line).
bool parseInto(std::string_view line, bool decimalComma, std::vector<double> &values, std::vector<std::string> *labels,
               std::vector<std::string> *words)
{
    values.clear();
    if (labels) labels->clear();
    if (words) words->clear();
    bool allNumbers = true;
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (i < n)
    {
        while (i < n && isSeparator(line[i], decimalComma)) ++i;
        if (i >= n) break;
        std::size_t j = i;
        while (j < n && !isSeparator(line[j], decimalComma)) ++j;
        std::string_view token = line.substr(i, j - i);
        i = j;
        std::string_view label;
        const std::size_t colon = token.find_first_of(":=");
        if (colon != std::string_view::npos)
        {
            label = token.substr(0, colon);
            token.remove_prefix(colon + 1);
        }
        double v = 0.0;
        if (parseNumber(token, decimalComma, v))
        {
            values.push_back(v);
            if (labels) labels->emplace_back(label);
        }
        else
        {
            allNumbers = false;
            if (words) words->emplace_back(colon == std::string_view::npos ? token : label);
        }
    }
    return allNumbers;
}

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}
} // namespace

std::optional<std::vector<double>> parseNumberLine(std::string_view line, bool decimalComma,
                                                   std::vector<std::string> *labels)
{
    std::vector<double> values;
    if (!parseInto(trim(line), decimalComma, values, labels, nullptr)) return std::nullopt;
    return values;
}

TextDecoder::TextDecoder(const TextFormat &format) : m_format(format)
{
    reset();
}

void TextDecoder::setFormat(const TextFormat &format)
{
    m_format = format;
    reset();
}

void TextDecoder::reset()
{
    m_carry.clear();
    m_overflow = false;
    m_channels = m_format.channelCount;
    m_names.clear();
    m_pendingDiscontinuity = false;
}

void TextDecoder::decode(std::span<const std::uint8_t> bytes, SampleBlock &out)
{
    m_statistics.bytes += bytes.size();
    const char *data = reinterpret_cast<const char *>(bytes.data());
    const std::size_t n = bytes.size();
    const std::size_t limit = std::max<std::size_t>(1, m_format.maximumLineLength);
    std::size_t start = 0;
    while (start < n)
    {
        // Next line terminator.
        std::size_t end = start;
        while (end < n && data[end] != '\n' && data[end] != '\r') ++end;
        const std::string_view piece(data + start, end - start);
        const bool complete = end < n;

        if (m_overflow)
        {
            if (complete) m_overflow = false; // the over-long line ends here
        }
        else if (m_carry.size() + piece.size() > limit)
        {
            ++m_statistics.overflows;
            ++m_statistics.rejected;
            if (m_statistics.frames > 0) m_pendingDiscontinuity = true;
            m_carry.clear();
            m_overflow = !complete;
        }
        else if (!complete)
        {
            m_carry.append(piece);
        }
        else if (m_carry.empty())
        {
            line(piece, out);
        }
        else
        {
            m_carry.append(piece);
            line(m_carry, out);
            m_carry.clear();
        }
        start = complete ? end + 1 : n;
    }
}

void TextDecoder::line(std::string_view text, SampleBlock &out)
{
    text = trim(text);
    if (text.empty()) return; // "\r\n" gives an empty line between the two terminators
    if (text.front() == '#')
    {
        ++m_statistics.comments;
        return;
    }
    std::vector<std::string> &words = m_words;
    const bool numeric = parseInto(text, m_format.decimalComma, m_values, &m_labels, &words);
    const bool dataStarted = m_statistics.frames > 0;
    if (!numeric)
    {
        if (!dataStarted && m_values.empty() && (m_channels == 0 || words.size() == m_channels))
        {
            // Header line: names of the columns.
            ++m_statistics.comments;
            m_names = words;
            if (m_channels == 0) m_channels = m_names.size();
            out.channelNames = m_names;
            return;
        }
        ++m_statistics.rejected;
        if (dataStarted) m_pendingDiscontinuity = true;
        return;
    }
    if (m_values.empty()) return;
    if (m_channels == 0) m_channels = m_values.size();
    if (m_values.size() != m_channels)
    {
        ++m_statistics.rejected;
        if (dataStarted) m_pendingDiscontinuity = true;
        return;
    }
    if (m_names.empty() && std::any_of(m_labels.begin(), m_labels.end(), [](const std::string &l) { return !l.empty(); }))
    {
        m_names = m_labels;
        for (std::size_t c = 0; c < m_names.size(); ++c)
            if (m_names[c].empty()) m_names[c] = "Voie " + std::to_string(c + 1);
        out.channelNames = m_names;
    }
    if (out.channelCount != m_channels)
    {
        // A block holds one channel count: start it afresh if the caller reused it with another.
        out.clear();
        out.channelCount = m_channels;
        out.channelNames = m_names;
    }
    if (m_pendingDiscontinuity)
    {
        out.discontinuities.push_back(out.frameCount());
        m_pendingDiscontinuity = false;
    }
    out.values.insert(out.values.end(), m_values.begin(), m_values.end());
    ++m_statistics.frames;
}
} // namespace qacq::io
