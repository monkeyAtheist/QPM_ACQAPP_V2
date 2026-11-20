#include "qacq/acquisition/stream_writer.h"

#include "qacq/model/json.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace qacq::acquisition
{
namespace
{
void put(std::string &out, std::uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::string csvNumber(double v, bool comma)
{
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    std::string s = model::formatNumber(v);
    if (comma) std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string title(const model::ChannelInfo &c, char separator)
{
    std::string t = c.unit.empty() ? c.name : c.name + " (" + c.unit + ")";
    if (t.find(separator) == std::string::npos && t.find('"') == std::string::npos) return t;
    std::string q = "\"";
    for (char ch : t)
    {
        if (ch == '"') q += '"';
        q += ch;
    }
    return q + "\"";
}
} // namespace

StreamWriter::~StreamWriter()
{
    close();
}

std::string StreamWriter::open(const std::filesystem::path &file, const std::vector<model::ChannelInfo> &channels,
                               double sampleRateHz, const StreamOptions &options)
{
    close();
    if (channels.empty()) return "Aucune voie à enregistrer.";
    if (!(sampleRateHz > 0.0) && (options.format == StreamFormat::Wav || options.timeColumn))
        return "La fréquence d'échantillonnage doit être connue pour cet enregistrement.";
    m_options = options;
    m_channels = channels.size();
    m_rate = sampleRateHz;
    m_frames = m_bytes = m_clipped = m_sinceFlush = 0;
    m_out.open(file, std::ios::binary | std::ios::trunc);
    if (!m_out) return "Impossible de créer le fichier d'enregistrement.";
    std::string head;
    if (options.format == StreamFormat::Csv)
    {
        const char sep = options.decimalComma ? ';' : ',';
        if (options.timeColumn) head += "temps (s)";
        for (std::size_t c = 0; c < channels.size(); ++c)
        {
            if (options.timeColumn || c > 0) head += sep;
            head += title(channels[c], sep);
        }
        head += "\r\n";
    }
    else
    {
        const int bits = options.wavFloat ? 32 : 16;
        const auto rate = static_cast<std::uint64_t>(std::llround(sampleRateHz));
        const std::uint64_t block = m_channels * static_cast<std::uint64_t>(bits / 8);
        head += "RIFF";
        put(head, 0xFFFFFFFFull, 4); // unknown until close()
        head += "WAVEfmt ";
        put(head, options.wavFloat ? 18 : 16, 4);
        put(head, options.wavFloat ? 3 : 1, 2);
        put(head, m_channels, 2);
        put(head, rate, 4);
        put(head, rate * block, 4);
        put(head, block, 2);
        put(head, static_cast<std::uint64_t>(bits), 2);
        if (options.wavFloat) put(head, 0, 2);
        head += "data";
        put(head, 0xFFFFFFFFull, 4);
    }
    m_out.write(head.data(), static_cast<std::streamsize>(head.size()));
    m_dataStart = head.size();
    m_bytes = head.size();
    if (!m_out) return "Écriture impossible (disque plein ou fichier protégé ?).";
    return {};
}

std::string StreamWriter::append(std::span<const double> interleaved)
{
    if (!m_out.is_open()) return "Aucun enregistrement en cours.";
    const std::size_t frames = interleaved.size() / m_channels;
    m_line.clear();
    if (m_options.format == StreamFormat::Csv)
    {
        const char sep = m_options.decimalComma ? ';' : ',';
        for (std::size_t f = 0; f < frames; ++f)
        {
            if (m_options.timeColumn) m_line += csvNumber(static_cast<double>(m_frames + f) / m_rate, m_options.decimalComma);
            for (std::size_t c = 0; c < m_channels; ++c)
            {
                if (m_options.timeColumn || c > 0) m_line += sep;
                m_line += csvNumber(interleaved[f * m_channels + c], m_options.decimalComma);
            }
            m_line += "\r\n";
        }
    }
    else
    {
        for (std::size_t i = 0; i < frames * m_channels; ++i)
        {
            const double v = interleaved[i];
            if (m_options.wavFloat)
            {
                put(m_line, std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
                continue;
            }
            double code = std::isnan(v) ? 0.0 : std::round(v / m_options.fullScale * 32768.0);
            if (code > 32767.0 || code < -32768.0)
            {
                ++m_clipped;
                code = std::clamp(code, -32768.0, 32767.0);
            }
            put(m_line, static_cast<std::uint64_t>(static_cast<std::int64_t>(code)), 2);
        }
    }
    m_out.write(m_line.data(), static_cast<std::streamsize>(m_line.size()));
    m_bytes += m_line.size();
    m_frames += frames;
    m_sinceFlush += frames;
    if (static_cast<double>(m_sinceFlush) >= std::max(1.0, m_rate))
    {
        m_out.flush(); // about once a second: little lost if the program stops abruptly
        m_sinceFlush = 0;
    }
    if (!m_out) return "Écriture impossible (disque plein ?) : l'enregistrement s'arrête.";
    return {};
}

std::string StreamWriter::close()
{
    if (!m_out.is_open()) return {};
    std::string error;
    if (m_options.format == StreamFormat::Wav)
    {
        const std::uint64_t data = m_bytes - m_dataStart;
        if (data & 1)
        {
            m_out.put('\0');
            ++m_bytes;
        }
        if (m_bytes - 8 <= 0xFFFFFFFFull)
        {
            std::string riff, size;
            put(riff, m_bytes - 8, 4);
            put(size, data, 4);
            m_out.seekp(4);
            m_out.write(riff.data(), 4);
            m_out.seekp(static_cast<std::streamoff>(m_dataStart - 4));
            m_out.write(size.data(), 4);
        }
        else
        {
            error = "Enregistrement de plus de 4 Go : le fichier WAV se relit jusqu'à la fin malgré tout.";
        }
    }
    m_out.close();
    if (m_out.fail() && error.empty()) error = "La fin de l'enregistrement n'a pas pu être écrite.";
    return error;
}
} // namespace qacq::acquisition
