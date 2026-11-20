#include "qacq/model/wav.h"

#include "file_names.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>

namespace qacq::model
{
namespace
{
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
// Tail of the KSDATAFORMAT_SUBTYPE GUIDs: {000000xx-0000-0010-8000-00aa00389b71}.
constexpr std::array<std::uint8_t, 14> kGuidTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                    0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

void put(std::string &out, std::uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::uint64_t get(const std::uint8_t *p, int bytes)
{
    std::uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

int bitsOf(WavEncoding e)
{
    switch (e)
    {
        case WavEncoding::Pcm16: return 16;
        case WavEncoding::Pcm24: return 24;
        case WavEncoding::Pcm32: return 32;
        case WavEncoding::Float32: return 32;
    }
    return 16;
}
} // namespace

std::string writeWav(std::ostream &out, const Signal &signal, const WavWriteOptions &options, std::vector<std::string> *warnings)
{
    const std::string problem = validateSignal(signal);
    if (!problem.empty()) return problem;
    const auto channels = signal.channels.size();
    if (channels > 65535) return "Trop de voies pour un fichier WAV.";
    double rate = std::round(signal.sampleRateHz);
    if (rate < 1.0 || rate > 4294967295.0) return "Fréquence d'échantillonnage hors de portée d'un fichier WAV.";
    if (rate != signal.sampleRateHz && warnings)
        warnings->push_back("Fréquence arrondie à " + formatNumber(rate) + " Hz (un fichier WAV n'en garde que la partie entière).");
    const int bits = bitsOf(options.encoding);
    const bool isFloat = options.encoding == WavEncoding::Float32;
    const std::uint64_t bytesPerSample = static_cast<std::uint64_t>(bits / 8);
    const std::uint64_t frames = signal.frameCount();
    const std::uint64_t dataBytes = frames * channels * bytesPerSample;
    const bool extensible = channels > 2 || bits > 16;
    const std::uint64_t fmtBytes = extensible ? 40 : (isFloat ? 18 : 16);
    const bool fact = isFloat || extensible;
    const std::uint64_t riffBytes = 4 + (8 + fmtBytes) + (fact ? 12 : 0) + (8 + dataBytes + (dataBytes & 1));
    if (riffBytes > 0xFFFFFFFFull) return "Signal trop long pour un fichier WAV (4 Go au plus).";

    double scale = options.fullScale;
    if (options.normalize)
    {
        double peak = 0.0;
        for (const auto &c : signal.data->channels)
            for (double v : c)
                if (std::isfinite(v)) peak = std::max(peak, std::abs(v));
        scale = peak > 0.0 ? peak : 1.0;
    }
    if (!(std::isfinite(scale) && scale > 0.0)) return "La pleine échelle doit être positive.";

    std::string header;
    header += "RIFF";
    put(header, riffBytes, 4);
    header += "WAVEfmt ";
    put(header, fmtBytes, 4);
    put(header, extensible ? kFormatExtensible : (isFloat ? kFormatFloat : kFormatPcm), 2);
    put(header, channels, 2);
    put(header, static_cast<std::uint64_t>(rate), 4);
    put(header, static_cast<std::uint64_t>(rate) * channels * bytesPerSample, 4);
    put(header, channels * bytesPerSample, 2);
    put(header, static_cast<std::uint64_t>(bits), 2);
    if (extensible)
    {
        put(header, 22, 2);                                  // cbSize
        put(header, static_cast<std::uint64_t>(bits), 2);    // valid bits
        put(header, channels == 1 ? 0x4 : channels == 2 ? 0x3 : 0, 4); // centre, or front left + right
        put(header, isFloat ? kFormatFloat : kFormatPcm, 2);
        header.append(reinterpret_cast<const char *>(kGuidTail.data()), kGuidTail.size());
    }
    else if (isFloat)
    {
        put(header, 0, 2); // cbSize
    }
    if (fact)
    {
        header += "fact";
        put(header, 4, 4);
        put(header, frames, 4);
    }
    header += "data";
    put(header, dataBytes, 4);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));

    const double maxCode = std::ldexp(1.0, bits - 1) - 1.0;
    const double minCode = -std::ldexp(1.0, bits - 1);
    std::uint64_t clipped = 0;
    std::string chunk;
    chunk.reserve(1 << 16);
    for (std::uint64_t f = 0; f < frames; ++f)
    {
        for (std::size_t c = 0; c < channels; ++c)
        {
            const double v = signal.data->channels[c][f] / scale;
            if (isFloat)
            {
                put(chunk, std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
                continue;
            }
            double code = std::isnan(v) ? 0.0 : std::round(v * (maxCode + 1.0));
            if (code > maxCode || code < minCode)
            {
                ++clipped;
                code = std::clamp(code, minCode, maxCode);
            }
            put(chunk, static_cast<std::uint64_t>(static_cast<std::int64_t>(code)), bits / 8);
        }
        if (chunk.size() >= (1 << 16) - 64)
        {
            out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            chunk.clear();
        }
    }
    if (dataBytes & 1) chunk.push_back('\0');
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    out.flush();
    if (clipped && warnings)
        warnings->push_back(std::to_string(clipped) + " échantillon(s) au-delà de la pleine échelle ont été écrêtés.");
    if (!out) return "Écriture impossible (disque plein ou fichier protégé ?).";
    return {};
}

std::string exportWav(const std::filesystem::path &file, const Signal &signal, const WavWriteOptions &options,
                      std::vector<std::string> *warnings)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return "Impossible de créer « " + detail::displayName(file) + " ».";
    return writeWav(out, signal, options, warnings);
}

ImportResult readWav(std::istream &in, const std::string &name)
{
    ImportResult result;
    auto fail = [&](const std::string &why) {
        result.error = why;
        return result;
    };
    in.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uint64_t>(std::max<std::streamoff>(0, in.tellg()));
    in.seekg(0, std::ios::beg);
    std::array<std::uint8_t, 12> riff{};
    in.read(reinterpret_cast<char *>(riff.data()), 12);
    if (!in) return fail("Fichier trop court pour un fichier WAV.");
    if (std::memcmp(riff.data(), "RF64", 4) == 0) return fail("Fichier WAV de plus de 4 Go (RF64) : non pris en charge.");
    if (std::memcmp(riff.data(), "RIFF", 4) != 0 || std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
        return fail("Ce n'est pas un fichier WAV.");

    std::uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    std::uint32_t rate = 0;
    bool haveFormat = false;
    std::uint64_t dataOffset = 0, dataBytes = 0;
    bool haveData = false;
    std::uint64_t position = 12;
    while (position + 8 <= fileSize && !haveData)
    {
        std::array<std::uint8_t, 8> head{};
        in.seekg(static_cast<std::streamoff>(position));
        in.read(reinterpret_cast<char *>(head.data()), 8);
        if (!in) break;
        const std::uint64_t size = get(head.data() + 4, 4);
        const std::uint64_t body = position + 8;
        if (std::memcmp(head.data(), "fmt ", 4) == 0)
        {
            if (size < 16) return fail("Description du format WAV incomplète.");
            std::array<std::uint8_t, 40> fmt{};
            in.read(reinterpret_cast<char *>(fmt.data()), static_cast<std::streamsize>(std::min<std::uint64_t>(size, 40)));
            if (!in) return fail("Description du format WAV illisible.");
            format = static_cast<std::uint16_t>(get(fmt.data(), 2));
            channels = static_cast<std::uint16_t>(get(fmt.data() + 2, 2));
            rate = static_cast<std::uint32_t>(get(fmt.data() + 4, 4));
            blockAlign = static_cast<std::uint16_t>(get(fmt.data() + 12, 2));
            bits = static_cast<std::uint16_t>(get(fmt.data() + 14, 2));
            if (format == kFormatExtensible)
            {
                if (size < 40) return fail("Format WAV étendu incomplet.");
                if (std::memcmp(fmt.data() + 26, kGuidTail.data(), kGuidTail.size()) != 0)
                    return fail("Format WAV étendu inconnu.");
                format = static_cast<std::uint16_t>(get(fmt.data() + 24, 2));
            }
            haveFormat = true;
        }
        else if (std::memcmp(head.data(), "data", 4) == 0)
        {
            dataOffset = body;
            dataBytes = size;
            if (dataOffset + dataBytes > fileSize)
            {
                // Recorder stopped before writing the sizes, or truncated copy.
                dataBytes = fileSize - dataOffset;
                result.warnings.emplace_back("Fichier WAV tronqué ou taille non renseignée : lu jusqu'à la fin du fichier.");
            }
            haveData = true;
        }
        position = body + size + (size & 1);
    }
    if (!haveFormat) return fail("Fichier WAV sans description du format.");
    if (!haveData) return fail("Fichier WAV sans données.");
    const bool isFloat = format == kFormatFloat;
    if (format != kFormatPcm && !isFloat) return fail("Codage WAV non pris en charge (format " + std::to_string(format) + ").");
    const bool supported = isFloat ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!supported) return fail("Résolution WAV non prise en charge (" + std::to_string(bits) + " bits).");
    if (channels == 0 || rate == 0) return fail("Fichier WAV sans voie ou sans fréquence d'échantillonnage.");
    const std::uint64_t bytesPerSample = bits / 8u;
    if (blockAlign != channels * bytesPerSample) return fail("Alignement des échantillons WAV incohérent.");

    const std::uint64_t frames = dataBytes / blockAlign;
    auto data = std::make_shared<SampleData>();
    data->channels.assign(channels, std::vector<double>(static_cast<std::size_t>(frames)));
    in.clear();
    in.seekg(static_cast<std::streamoff>(dataOffset));
    std::vector<std::uint8_t> buffer;
    const std::uint64_t framesPerChunk = std::max<std::uint64_t>(1, (1u << 20) / blockAlign);
    const double intScale = std::ldexp(1.0, -(static_cast<int>(bits) - 1));
    for (std::uint64_t first = 0; first < frames; first += framesPerChunk)
    {
        const std::uint64_t count = std::min(framesPerChunk, frames - first);
        buffer.resize(static_cast<std::size_t>(count * blockAlign));
        in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!in) return fail("Échantillons WAV illisibles.");
        for (std::uint64_t f = 0; f < count; ++f)
        {
            for (std::uint16_t c = 0; c < channels; ++c)
            {
                const std::uint8_t *p = buffer.data() + f * blockAlign + c * bytesPerSample;
                double v = 0.0;
                if (isFloat)
                {
                    v = bits == 32 ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(get(p, 4))))
                                   : std::bit_cast<double>(get(p, 8));
                }
                else if (bits == 8)
                {
                    v = (static_cast<double>(p[0]) - 128.0) / 128.0;
                }
                else
                {
                    const std::uint64_t u = get(p, static_cast<int>(bytesPerSample));
                    const int shift = 64 - static_cast<int>(bits);
                    const auto s = static_cast<std::int64_t>(u << shift) >> shift; // sign extension
                    v = static_cast<double>(s) * intScale;
                }
                data->channels[c][static_cast<std::size_t>(first + f)] = v;
            }
        }
    }

    Signal &signal = result.signal;
    signal.name = name.empty() ? "Import WAV" : name;
    signal.origin = Origin::Imported;
    signal.sampleRateHz = rate;
    signal.channels = defaultChannels(channels);
    signal.data = std::move(data);
    signal.notes = "Pleine échelle du fichier WAV = 1.";
    signal.provenance = Json::object();
    signal.provenance["import"] = "wav";
    signal.provenance["encoding"] = std::string(isFloat ? "float " : "PCM ") + std::to_string(bits) + " bits";
    return result;
}

ImportResult importWav(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        ImportResult r;
        r.error = "Impossible d'ouvrir « " + detail::displayName(file) + " ».";
        return r;
    }
    const std::u8string stem = file.stem().u8string();
    ImportResult r = readWav(in, std::string(reinterpret_cast<const char *>(stem.data()), stem.size()));
    if (r.ok()) r.signal.provenance["file"] = detail::displayName(file);
    return r;
}
} // namespace qacq::model
