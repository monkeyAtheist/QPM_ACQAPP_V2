#include "qacq/model/session_file.h"

#include "file_names.h"
#include "qacq/io/crc.h"

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
using detail::displayName;

namespace
{
constexpr char kMagic[8] = {'Q', 'A', 'C', 'Q', 'S', 'E', 'S', 'S'};
constexpr std::size_t kFixedHeader = 20;
constexpr std::uint64_t kMaximumHeaderBytes = 256ull * 1024 * 1024;
constexpr std::size_t kChunk = 1 << 16; // samples converted at a time

std::uint64_t align8(std::uint64_t n)
{
    return (n + 7) & ~std::uint64_t{7};
}

void putLe(std::uint8_t *p, std::uint64_t v, std::size_t bytes)
{
    for (std::size_t i = 0; i < bytes; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint64_t getLe(const std::uint8_t *p, std::size_t bytes)
{
    std::uint64_t v = 0;
    for (std::size_t i = bytes; i-- > 0;) v = (v << 8) | p[i];
    return v;
}

bool exactFloat(const std::vector<double> &values)
{
    for (double v : values)
        if (!std::isnan(v) && static_cast<double>(static_cast<float>(v)) != v) return false;
    return true;
}

// Little-endian bytes of values[first, first + count) in out.
void encode(const std::vector<double> &values, std::size_t first, std::size_t count, bool asFloat,
            std::vector<std::uint8_t> &out)
{
    const std::size_t size = asFloat ? 4 : 8;
    out.resize(count * size);
    for (std::size_t i = 0; i < count; ++i)
    {
        const double v = values[first + i];
        if (asFloat) putLe(out.data() + 4 * i, std::bit_cast<std::uint32_t>(static_cast<float>(v)), 4);
        else putLe(out.data() + 8 * i, std::bit_cast<std::uint64_t>(v), 8);
    }
}

std::string hex32(std::uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i, v >>= 4) s[static_cast<std::size_t>(i)] = digits[v & 0xF];
    return s;
}

bool parseHex32(const std::string &s, std::uint32_t &v)
{
    if (s.size() != 8) return false;
    v = 0;
    for (char c : s)
    {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
    }
    return true;
}

struct ChannelPlan
{
    const std::vector<double> *values = nullptr;
    bool asFloat = false;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    std::uint32_t crc = 0;
};
} // namespace

Json signalToJson(const Signal &s)
{
    Json j = Json::object();
    j["id"] = s.id;
    j["name"] = s.name;
    j["origin"] = originKey(s.origin);
    j["representation"] = s.representation == Representation::IQ ? "iq" : "real";
    j["sampleRateHz"] = s.sampleRateHz;
    j["startSeconds"] = s.startSeconds;
    if (!s.createdUtc.empty()) j["created"] = s.createdUtc;
    if (!s.notes.empty()) j["notes"] = s.notes;
    Json channels = Json::array();
    for (const ChannelInfo &c : s.channels)
    {
        Json cj = Json::object();
        cj["name"] = c.name;
        cj["unit"] = c.unit;
        channels.push_back(std::move(cj));
    }
    j["channels"] = std::move(channels);
    j["frames"] = s.frameCount();
    Json breaks = Json::array();
    if (s.data)
        for (std::size_t d : s.data->discontinuities) breaks.push_back(d);
    j["discontinuities"] = std::move(breaks);
    if (!s.provenance.isNull()) j["provenance"] = s.provenance;
    return j;
}

bool signalFromJson(const Json &j, Signal &s, std::string *error)
{
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return false;
    };
    if (!j.isObject()) return fail("description de signal illisible");
    s.id = static_cast<std::uint64_t>(std::max(0.0, j["id"].toNumber(0.0)));
    s.name = j["name"].toString("Signal");
    if (!originFromKey(j["origin"].toString("generated"), s.origin)) s.origin = Origin::Imported;
    s.representation = j["representation"].toString("real") == "iq" ? Representation::IQ : Representation::Real;
    s.sampleRateHz = j["sampleRateHz"].toNumber(0.0);
    s.startSeconds = j["startSeconds"].toNumber(0.0);
    s.createdUtc = j["created"].toString();
    s.notes = j["notes"].toString();
    s.provenance = j["provenance"];
    s.channels.clear();
    for (const Json &c : j["channels"].items()) s.channels.push_back({c["name"].toString(), c["unit"].toString()});
    if (s.channels.empty()) return fail("« " + s.name + " » : aucune voie décrite");
    return true;
}

std::string writeSession(std::ostream &out, const SessionContent &content, const SessionWriteOptions &options)
{
    // Plan: encoding, place and checksum of every channel (first pass over the samples).
    Json header = Json::object();
    header["format"] = "QTacquisitionApp session";
    header["version"] = kSessionFormatVersion;
    if (!content.application.empty()) header["application"] = content.application;
    if (!content.savedUtc.empty()) header["saved"] = content.savedUtc;
    header["settings"] = content.settings.isObject() ? content.settings : Json::object();
    Json list = Json::array();
    std::vector<ChannelPlan> plan;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> buffer;
    for (const SignalPtr &signal : content.signalList)
    {
        if (!signal) continue;
        const std::string problem = validateSignal(*signal);
        if (!problem.empty()) return "« " + signal->name + " » : " + problem;
        Json sj = signalToJson(*signal);
        Json data = Json::array();
        for (const auto &values : signal->data->channels)
        {
            ChannelPlan p;
            p.values = &values;
            p.asFloat = options.float32WhenExact && exactFloat(values);
            p.offset = offset;
            p.bytes = values.size() * (p.asFloat ? 4u : 8u);
            io::Crc crc(io::kCrc32);
            for (std::size_t first = 0; first < values.size(); first += kChunk)
            {
                encode(values, first, std::min(kChunk, values.size() - first), p.asFloat, buffer);
                crc.update(buffer);
            }
            p.crc = crc.value();
            Json dj = Json::object();
            dj["offset"] = p.offset;
            dj["encoding"] = p.asFloat ? "f32le" : "f64le";
            dj["bytes"] = p.bytes;
            dj["crc32"] = hex32(p.crc);
            data.push_back(std::move(dj));
            offset = align8(offset + p.bytes);
            plan.push_back(p);
        }
        sj["data"] = std::move(data);
        list.push_back(std::move(sj));
    }
    header["signals"] = std::move(list);

    const std::string text = header.dump(2) + "\n";
    std::array<std::uint8_t, kFixedHeader> fixed{};
    std::memcpy(fixed.data(), kMagic, 8);
    putLe(fixed.data() + 8, kSessionFormatVersion, 4);
    putLe(fixed.data() + 12, text.size(), 8);
    out.write(reinterpret_cast<const char *>(fixed.data()), static_cast<std::streamsize>(fixed.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    const std::uint64_t start = kFixedHeader + text.size();
    static const char zeros[8] = {};
    out.write(zeros, static_cast<std::streamsize>(align8(start) - start));

    std::uint64_t written = 0;
    for (const ChannelPlan &p : plan)
    {
        out.write(zeros, static_cast<std::streamsize>(p.offset - written)); // padding of the previous channel
        written = p.offset;
        for (std::size_t first = 0; first < p.values->size(); first += kChunk)
        {
            encode(*p.values, first, std::min(kChunk, p.values->size() - first), p.asFloat, buffer);
            out.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        }
        written += p.bytes;
        if (!out) return "Écriture impossible (disque plein ou fichier protégé ?).";
    }
    out.write(zeros, static_cast<std::streamsize>(align8(written) - written));
    out.flush();
    if (!out) return "Écriture impossible (disque plein ou fichier protégé ?).";
    return {};
}

std::string saveSession(const std::filesystem::path &file, const SessionContent &content,
                        const SessionWriteOptions &options)
{
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return "Impossible de créer le fichier « " + displayName(file) + " ».";
        const std::string error = writeSession(out, content, options);
        out.close();
        if (!error.empty() || !out)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return error.empty() ? "Écriture impossible (disque plein ou fichier protégé ?)." : error;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    if (ec)
    {
        // Some systems refuse to rename onto an existing file.
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::filesystem::rename(temporary, file, ec);
    }
    if (ec)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return "Impossible de remplacer « " + displayName(file) + " » : " + ec.message();
    }
    return {};
}

std::optional<SessionContent> readSession(std::istream &in, std::string *error)
{
    auto fail = [&](const std::string &why) -> std::optional<SessionContent> {
        if (error) *error = why;
        return std::nullopt;
    };
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    in.seekg(0, std::ios::beg);
    if (end < static_cast<std::streamoff>(kFixedHeader)) return fail("Ce n'est pas un fichier de session (trop court).");
    const auto fileSize = static_cast<std::uint64_t>(end);

    std::array<std::uint8_t, kFixedHeader> fixed{};
    in.read(reinterpret_cast<char *>(fixed.data()), static_cast<std::streamsize>(fixed.size()));
    if (!in || std::memcmp(fixed.data(), kMagic, 8) != 0) return fail("Ce n'est pas un fichier de session QTacquisitionApp.");
    const auto version = static_cast<std::uint32_t>(getLe(fixed.data() + 8, 4));
    if (version == 0 || version > kSessionFormatVersion)
        return fail("Session enregistrée par une version plus récente de l'application (format " + std::to_string(version) +
                    ") : mettez l'application à jour.");
    const std::uint64_t headerBytes = getLe(fixed.data() + 12, 8);
    if (headerBytes > kMaximumHeaderBytes || kFixedHeader + headerBytes > fileSize)
        return fail("Fichier de session tronqué ou endommagé (en-tête).");
    std::string text(static_cast<std::size_t>(headerBytes), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in) return fail("Fichier de session illisible (en-tête).");
    std::string parseError;
    const auto header = Json::parse(text, &parseError);
    if (!header) return fail("En-tête de session illisible : " + parseError);
    if ((*header)["format"].toString() != "QTacquisitionApp session") return fail("Ce n'est pas un fichier de session QTacquisitionApp.");

    SessionContent content;
    content.settings = (*header)["settings"].isObject() ? (*header)["settings"] : Json::object();
    content.application = (*header)["application"].toString();
    content.savedUtc = (*header)["saved"].toString();
    const std::uint64_t dataStart = align8(kFixedHeader + headerBytes);
    std::vector<std::uint8_t> buffer;
    for (const Json &sj : (*header)["signals"].items())
    {
        Signal signal;
        std::string why;
        if (!signalFromJson(sj, signal, &why)) return fail("Session endommagée : " + why + ".");
        const double framesNumber = sj["frames"].toNumber(-1.0);
        if (!(framesNumber >= 0.0) || framesNumber > 9.0e15) return fail("« " + signal.name + " » : nombre d'échantillons illisible.");
        const auto frames = static_cast<std::uint64_t>(framesNumber);
        const Json::Array &locations = sj["data"].items();
        if (locations.size() != signal.channels.size())
            return fail("« " + signal.name + " » : les données ne correspondent pas aux voies décrites.");
        auto data = std::make_shared<SampleData>();
        for (std::size_t c = 0; c < locations.size(); ++c)
        {
            const Json &dj = locations[c];
            const std::string encoding = dj["encoding"].toString();
            const bool asFloat = encoding == "f32le";
            if (!asFloat && encoding != "f64le") return fail("« " + signal.name + " » : codage des échantillons inconnu (" + encoding + ").");
            const std::uint64_t size = asFloat ? 4 : 8;
            const auto offset = static_cast<std::uint64_t>(dj["offset"].toNumber(-1.0));
            const auto bytes = static_cast<std::uint64_t>(dj["bytes"].toNumber(-1.0));
            std::uint32_t expected = 0;
            if (dj["offset"].toNumber(-1.0) < 0.0 || bytes != frames * size || !parseHex32(dj["crc32"].toString(), expected))
                return fail("« " + signal.name + " » : emplacement des échantillons illisible.");
            if (dataStart + offset + bytes > fileSize)
                return fail("Fichier de session tronqué : les échantillons de « " + signal.name + " » manquent.");
            in.seekg(static_cast<std::streamoff>(dataStart + offset), std::ios::beg);
            std::vector<double> values(static_cast<std::size_t>(frames));
            io::Crc crc(io::kCrc32);
            for (std::size_t first = 0; first < values.size(); first += kChunk)
            {
                const std::size_t count = std::min(kChunk, values.size() - first);
                buffer.resize(count * size);
                in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                if (!in) return fail("Fichier de session illisible : échantillons de « " + signal.name + " ».");
                crc.update(buffer);
                for (std::size_t i = 0; i < count; ++i)
                {
                    values[first + i] = asFloat
                                            ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(getLe(buffer.data() + 4 * i, 4))))
                                            : std::bit_cast<double>(getLe(buffer.data() + 8 * i, 8));
                }
            }
            if (crc.value() != expected)
                return fail("Les échantillons de « " + signal.name + " » (voie " + std::to_string(c + 1) +
                            ") sont endommagés : somme de contrôle incorrecte.");
            data->channels.push_back(std::move(values));
        }
        for (const Json &d : sj["discontinuities"].items()) data->discontinuities.push_back(static_cast<std::size_t>(d.toNumber()));
        signal.data = std::move(data);
        const std::string problem = validateSignal(signal);
        if (!problem.empty()) return fail("« " + signal.name + " » : " + problem);
        content.signalList.push_back(std::make_shared<const Signal>(std::move(signal)));
    }
    return content;
}

std::optional<SessionContent> loadSession(const std::filesystem::path &file, std::string *error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        if (error) *error = "Impossible d'ouvrir « " + displayName(file) + " ».";
        return std::nullopt;
    }
    return readSession(in, error);
}
} // namespace qacq::model
