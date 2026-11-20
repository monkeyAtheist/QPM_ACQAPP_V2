#include "qacq/acquisition/simulator.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace qacq::acquisition
{
namespace
{
std::string fixed(double v, int decimals, bool comma)
{
    std::array<char, 64> buffer{};
    const auto r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v, std::chars_format::fixed, std::clamp(decimals, 0, 12));
    std::string s(buffer.data(), r.ptr);
    if (comma) std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// SCPI style: +1.234560E+00
std::string scientific(double v)
{
    std::array<char, 64> buffer{};
    const auto r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v, std::chars_format::scientific, 6);
    std::string s(buffer.data(), r.ptr);
    std::replace(s.begin(), s.end(), 'e', 'E');
    if (!s.empty() && s.front() != '-') s.insert(s.begin(), '+');
    return s;
}

std::string upper(std::string_view text)
{
    std::string s;
    for (char c : text)
        if (c != ' ' && c != '\r' && c != '\n') s += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    return s;
}
} // namespace

SimulatedDevice::SimulatedDevice(const SimulatedSettings &settings, const DecoderSettings &output, double sampleRateHz)
    : m_settings(settings), m_output(output), m_rate(sampleRateHz > 0.0 ? sampleRateHz : 1.0),
      m_noise(settings.seed), m_defects(settings.seed ^ 0x5DEECE66Dull)
{
    if (m_settings.channels.empty()) m_settings.channels.emplace_back();
    m_clockRate = m_rate * (1.0 + m_settings.rateErrorPpm * 1e-6);
}

double SimulatedDevice::cleanValue(std::size_t channel, std::uint64_t frame) const
{
    if (channel >= m_settings.channels.size()) return 0.0;
    const SimulatedChannel &c = m_settings.channels[channel];
    if (c.waveform == dsp::Waveform::WhiteNoise) return c.offset;
    if (c.waveform == dsp::Waveform::Dc) return c.offset + c.amplitude;
    const double cycles = c.frequencyHz * static_cast<double>(frame) / m_rate + c.phaseDegrees / 360.0;
    return c.offset + c.amplitude * dsp::periodicValue(c.waveform, cycles - std::floor(cycles), c.dutyCycle);
}

double SimulatedDevice::value(std::size_t channel, std::uint64_t frame)
{
    const SimulatedChannel &c = m_settings.channels[channel];
    double v = cleanValue(channel, frame);
    if (c.waveform == dsp::Waveform::WhiteNoise) v += c.amplitude * m_noise.normal();
    if (c.noise > 0.0) v += c.noise * m_noise.normal();
    return v;
}

void SimulatedDevice::damage(std::vector<std::uint8_t> &bytes, std::size_t from)
{
    if (!(m_settings.byteErrorRate > 0.0)) return;
    for (std::size_t i = from; i < bytes.size(); ++i)
        if (m_defects.uniform() < m_settings.byteErrorRate) bytes[i] ^= static_cast<std::uint8_t>(1u << (m_defects.next() % 8));
}

void SimulatedDevice::appendFrame(std::uint64_t frame, std::vector<std::uint8_t> &out)
{
    const std::size_t channels = channelCount();
    const bool lose = m_settings.frameLossRate > 0.0 && m_defects.uniform() < m_settings.frameLossRate;
    switch (m_output.kind)
    {
        case DecoderKind::Text:
        case DecoderKind::Scpi:
        {
            const bool scpi = m_output.kind == DecoderKind::Scpi;
            const bool comma = !scpi && m_output.text.decimalComma;
            std::string line;
            for (std::size_t c = 0; c < channels; ++c)
            {
                if (c) line += comma ? ";" : ",";
                if (m_settings.textLabels && !scpi) line += "voie" + std::to_string(c + 1) + ":";
                const double v = value(c, frame);
                line += scpi ? scientific(v) : fixed(v, m_settings.textDecimals, comma);
            }
            line += scpi ? "\n" : "\r\n";
            if (lose)
            {
                ++m_lost;
                return;
            }
            out.insert(out.end(), line.begin(), line.end());
            return;
        }
        case DecoderKind::RawBinary:
        {
            const std::size_t size = io::sampleSize(m_output.raw.type);
            const std::size_t at = out.size();
            out.resize(at + size * channels);
            for (std::size_t c = 0; c < channels; ++c) io::writeSample(value(c, frame), m_output.raw, out.data() + at + c * size);
            if (lose)
            {
                ++m_lost;
                out.resize(at);
            }
            return;
        }
        case DecoderKind::FramedBinary:
        {
            // One packet every samplesPerChannel frames: built when its last frame is reached.
            const std::size_t per = std::max<std::size_t>(1, m_output.frame.samplesPerChannel);
            if ((frame + 1) % per != 0) return;
            const std::uint64_t first = frame + 1 - per;
            std::vector<double> values;
            values.reserve(per * channels);
            for (std::size_t s = 0; s < per; ++s)
                for (std::size_t c = 0; c < channels; ++c) values.push_back(value(c, first + s));
            if (lose)
            {
                m_lost += per;
                return;
            }
            const auto packet = io::encodeFrame(m_output.frame, static_cast<std::uint32_t>(frame / per), values);
            out.insert(out.end(), packet.begin(), packet.end());
            return;
        }
    }
}

std::vector<std::uint8_t> SimulatedDevice::produce(std::size_t frames)
{
    std::vector<std::uint8_t> out;
    for (std::size_t k = 0; k < frames; ++k) appendFrame(m_next++, out);
    damage(out, 0);
    return out;
}

std::vector<std::uint8_t> SimulatedDevice::produceFor(double seconds)
{
    if (!(seconds > 0.0)) return {};
    const double frames = seconds * m_clockRate + m_carry;
    const double whole = std::floor(frames);
    m_carry = frames - whole;
    return produce(static_cast<std::size_t>(whole));
}

std::string SimulatedDevice::answer(std::string_view command)
{
    const std::string c = upper(command);
    if (c.empty()) return {};
    if (c == "*IDN?") return "QTacquisitionApp,Simulateur,0,2.0.0\n";
    if (c == "*RST" || c == "*CLS")
    {
        m_errors.clear();
        return {};
    }
    if (c == "SYST:ERR?" || c == "SYSTEM:ERROR?" || c == "SYST:ERR:NEXT?")
    {
        if (m_errors.empty()) return "+0,\"No error\"\n";
        std::string e = m_errors.front();
        m_errors.erase(m_errors.begin());
        return e + "\n";
    }
    if (c.rfind("MEAS", 0) == 0 || c.rfind("READ", 0) == 0 || c.rfind(":MEAS", 0) == 0 || c == "FETC?")
    {
        std::string line;
        for (std::size_t ch = 0; ch < channelCount(); ++ch)
        {
            if (ch) line += ',';
            line += scientific(value(ch, m_next));
        }
        ++m_next;
        return line + "\n";
    }
    if (c == ":WAV:DATA?" || c == "WAV:DATA?" || c == ":WAVEFORM:DATA?" || c == "CURV?" || c == "CURVE?")
    {
        const io::SampleFormat &format = m_output.scpi.block;
        const std::size_t size = io::sampleSize(format.type);
        const std::size_t channels = channelCount();
        std::string data(waveformPoints * channels * size, '\0');
        for (std::size_t f = 0; f < waveformPoints; ++f)
        {
            for (std::size_t ch = 0; ch < channels; ++ch)
                io::writeSample(value(ch, m_next), format,
                                reinterpret_cast<std::uint8_t *>(data.data()) + (f * channels + ch) * size);
            ++m_next;
        }
        return io::blockHeader(data.size()) + data + "\n";
    }
    if (c.back() != '?') return {}; // a setting command: accepted silently
    m_errors.emplace_back("-113,\"Undefined header\"");
    return {};
}
} // namespace qacq::acquisition
