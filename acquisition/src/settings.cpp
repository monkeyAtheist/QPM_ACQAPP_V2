#include "qacq/acquisition/settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace qacq::acquisition
{
using model::Json;

namespace
{
template <typename E, std::size_t N>
std::string_view keyOf(E value, const std::pair<E, const char *> (&table)[N]) noexcept
{
    for (const auto &entry : table)
        if (entry.first == value) return entry.second;
    return table[0].second;
}

template <typename E, std::size_t N>
bool valueOf(std::string_view key, E &value, const std::pair<E, const char *> (&table)[N]) noexcept
{
    for (const auto &entry : table)
    {
        if (key == entry.second)
        {
            value = entry.first;
            return true;
        }
    }
    return false;
}

const std::pair<SourceKind, const char *> kSources[] = {
    {SourceKind::Serial, "serial"}, {SourceKind::TcpClient, "tcp"}, {SourceKind::Udp, "udp"},
    {SourceKind::Scpi, "scpi"},     {SourceKind::Audio, "audio"},   {SourceKind::Simulated, "simulated"}};
const std::pair<Parity, const char *> kParities[] = {
    {Parity::None, "none"}, {Parity::Even, "even"}, {Parity::Odd, "odd"}, {Parity::Mark, "mark"}, {Parity::Space, "space"}};
const std::pair<FlowControl, const char *> kFlows[] = {
    {FlowControl::None, "none"}, {FlowControl::Hardware, "hardware"}, {FlowControl::Software, "software"}};
const std::pair<ScpiTransport, const char *> kTransports[] = {{ScpiTransport::Tcp, "tcp"}, {ScpiTransport::Visa, "visa"}};
const std::pair<AudioSampleType, const char *> kAudioTypes[] = {
    {AudioSampleType::Int16, "int16"}, {AudioSampleType::Int32, "int32"}, {AudioSampleType::Float32, "float32"}};
const std::pair<DecoderKind, const char *> kDecoders[] = {
    {DecoderKind::Text, "text"}, {DecoderKind::RawBinary, "raw"}, {DecoderKind::FramedBinary, "frames"}, {DecoderKind::Scpi, "scpi"}};
const std::pair<io::SampleType, const char *> kSampleTypes[] = {
    {io::SampleType::Int8, "int8"},     {io::SampleType::UInt8, "uint8"},   {io::SampleType::Int16, "int16"},
    {io::SampleType::UInt16, "uint16"}, {io::SampleType::Int32, "int32"},   {io::SampleType::UInt32, "uint32"},
    {io::SampleType::Float32, "float32"}, {io::SampleType::Float64, "float64"}};
const std::pair<io::ByteOrder, const char *> kOrders[] = {{io::ByteOrder::LittleEndian, "le"}, {io::ByteOrder::BigEndian, "be"}};
const std::pair<io::CounterField, const char *> kCounters[] = {
    {io::CounterField::None, "none"}, {io::CounterField::UInt8, "u8"}, {io::CounterField::UInt16, "u16"}};
const std::pair<io::Checksum, const char *> kChecksums[] = {
    {io::Checksum::None, "none"},           {io::Checksum::Sum8, "sum8"},
    {io::Checksum::Xor8, "xor8"},           {io::Checksum::Crc8, "crc8"},
    {io::Checksum::Crc8Maxim, "crc8-maxim"}, {io::Checksum::Crc16Ccitt, "crc16-ccitt"},
    {io::Checksum::Crc16Xmodem, "crc16-xmodem"}, {io::Checksum::Crc16Modbus, "crc16-modbus"},
    {io::Checksum::Crc32, "crc32"}};
const std::pair<dsp::Waveform, const char *> kWaveforms[] = {
    {dsp::Waveform::Sine, "sine"},         {dsp::Waveform::Square, "square"}, {dsp::Waveform::Triangle, "triangle"},
    {dsp::Waveform::Sawtooth, "sawtooth"}, {dsp::Waveform::Pulse, "pulse"},   {dsp::Waveform::Dc, "dc"},
    {dsp::Waveform::WhiteNoise, "noise"}};

// Reads a member if present and of the right type; records the first problem.
struct Reader
{
    std::string error;

    void number(const Json &j, std::string_view key, double &out)
    {
        if (!j.contains(key)) return;
        if (j[key].isNumber()) out = j[key].toNumber();
        else wrong(key);
    }
    void integer(const Json &j, std::string_view key, int &out)
    {
        double v = out;
        number(j, key, v);
        if (std::isfinite(v) && v >= -2147483648.0 && v <= 2147483647.0) out = static_cast<int>(std::lround(v));
        else wrong(key);
    }
    void size(const Json &j, std::string_view key, std::size_t &out)
    {
        double v = static_cast<double>(out);
        number(j, key, v);
        if (std::isfinite(v) && v >= 0.0 && v <= 9.0e15) out = static_cast<std::size_t>(std::llround(v));
        else wrong(key);
    }
    void boolean(const Json &j, std::string_view key, bool &out)
    {
        if (!j.contains(key)) return;
        if (j[key].isBool()) out = j[key].toBool();
        else wrong(key);
    }
    void text(const Json &j, std::string_view key, std::string &out)
    {
        if (!j.contains(key)) return;
        if (j[key].isString()) out = j[key].toString();
        else wrong(key);
    }
    template <typename E, std::size_t N>
    void choice(const Json &j, std::string_view key, E &out, const std::pair<E, const char *> (&table)[N])
    {
        if (!j.contains(key)) return;
        if (!valueOf(j[key].toString(), out, table)) wrong(key);
    }
    void wrong(std::string_view key)
    {
        if (error.empty()) error = "Réglage « " + std::string(key) + " » illisible.";
    }
};

Json sampleFormatToJson(const io::SampleFormat &f)
{
    Json j = Json::object();
    j["type"] = keyOf(f.type, kSampleTypes);
    j["byteOrder"] = keyOf(f.order, kOrders);
    j["scale"] = f.scale;
    j["offset"] = f.offset;
    return j;
}

void sampleFormatFromJson(const Json &j, io::SampleFormat &f, Reader &r)
{
    r.choice(j, "type", f.type, kSampleTypes);
    r.choice(j, "byteOrder", f.order, kOrders);
    r.number(j, "scale", f.scale);
    r.number(j, "offset", f.offset);
}
} // namespace

std::string_view sourceKey(SourceKind kind) noexcept
{
    return keyOf(kind, kSources);
}

std::string_view sourceName(SourceKind kind) noexcept
{
    switch (kind)
    {
        case SourceKind::Serial: return "Port série";
        case SourceKind::TcpClient: return "Réseau TCP";
        case SourceKind::Udp: return "Réseau UDP";
        case SourceKind::Scpi: return "Instrument SCPI";
        case SourceKind::Audio: return "Carte son";
        case SourceKind::Simulated: return "Simulateur";
    }
    return "?";
}

double announcedRate(const AcquisitionSettings &settings) noexcept
{
    if (settings.source == SourceKind::Audio) return static_cast<double>(settings.audio.sampleRate);
    return settings.declaredSampleRateHz > 0.0 ? settings.declaredSampleRateHz : 0.0;
}

std::string_view defaultSignalName(SourceKind kind) noexcept
{
    switch (kind)
    {
        case SourceKind::Simulated: return "Essai du simulateur";
        case SourceKind::Audio: return "Enregistrement";
        default: return "Acquisition";
    }
}

std::string nameForSource(const std::string &name, SourceKind source)
{
    if (name.empty()) return std::string(defaultSignalName(source));
    for (SourceKind other : {SourceKind::Serial, SourceKind::TcpClient, SourceKind::Udp, SourceKind::Scpi,
                             SourceKind::Audio, SourceKind::Simulated})
        if (name == defaultSignalName(other)) return std::string(defaultSignalName(source));
    return name;
}

bool sourceFromKey(std::string_view key, SourceKind &kind) noexcept
{
    return valueOf(key, kind, kSources);
}

std::string_view decoderKey(DecoderKind kind) noexcept
{
    return keyOf(kind, kDecoders);
}

std::string_view decoderName(DecoderKind kind) noexcept
{
    switch (kind)
    {
        case DecoderKind::Text: return "Lignes de texte";
        case DecoderKind::RawBinary: return "Binaire brut";
        case DecoderKind::FramedBinary: return "Trames binaires";
        case DecoderKind::Scpi: return "Réponses SCPI";
    }
    return "?";
}

std::size_t DecoderSettings::channelCount() const noexcept
{
    switch (kind)
    {
        case DecoderKind::Text: return text.channelCount;
        case DecoderKind::RawBinary: return rawChannels;
        case DecoderKind::FramedBinary: return frame.channels;
        case DecoderKind::Scpi: return scpi.channels;
    }
    return 0;
}

std::string hexBytes(const std::vector<std::uint8_t> &bytes)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i) s += ' ';
        s += digits[bytes[i] >> 4];
        s += digits[bytes[i] & 0xF];
    }
    return s;
}

bool parseHexBytes(std::string_view text, std::vector<std::uint8_t> &bytes)
{
    std::vector<std::uint8_t> out;
    std::string digits;
    auto flush = [&]() -> bool {
        if (digits.empty()) return true;
        if (digits.size() > 2) return false;
        out.push_back(static_cast<std::uint8_t>(std::stoul(digits, nullptr, 16)));
        digits.clear();
        return true;
    };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && digits.empty())
        {
            ++i;
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c)))
        {
            digits += c;
            if (digits.size() == 2 && !flush()) return false;
        }
        else if (c == ' ' || c == ',' || c == ';' || c == ':' || c == '-')
        {
            if (!flush()) return false;
        }
        else
        {
            return false;
        }
    }
    if (!flush()) return false;
    bytes = std::move(out);
    return true;
}

Json frameFormatToJson(const io::FrameFormat &f)
{
    Json j = Json::object();
    j["sync"] = hexBytes(f.sync);
    j["counter"] = keyOf(f.counter, kCounters);
    j["sample"] = sampleFormatToJson(f.sample);
    j["channels"] = f.channels;
    j["samplesPerChannel"] = f.samplesPerChannel;
    j["checksum"] = keyOf(f.checksum, kChecksums);
    j["checksumCoversSync"] = f.checksumCoversSync;
    return j;
}

bool frameFormatFromJson(const Json &j, io::FrameFormat &f, std::string *error)
{
    Reader r;
    if (j.contains("sync"))
    {
        std::vector<std::uint8_t> sync;
        if (j["sync"].isString() && parseHexBytes(j["sync"].toString(), sync)) f.sync = sync;
        else r.wrong("sync");
    }
    r.choice(j, "counter", f.counter, kCounters);
    if (j.contains("sample")) sampleFormatFromJson(j["sample"], f.sample, r);
    r.size(j, "channels", f.channels);
    r.size(j, "samplesPerChannel", f.samplesPerChannel);
    r.choice(j, "checksum", f.checksum, kChecksums);
    r.boolean(j, "checksumCoversSync", f.checksumCoversSync);
    if (error) *error = r.error;
    return r.error.empty();
}

Json toJson(const AcquisitionSettings &s)
{
    Json j = Json::object();
    j["name"] = s.name;
    j["source"] = sourceKey(s.source);

    Json serial = Json::object();
    serial["port"] = s.serial.port;
    serial["baudRate"] = s.serial.baudRate;
    serial["dataBits"] = s.serial.dataBits;
    serial["parity"] = keyOf(s.serial.parity, kParities);
    serial["stopBits"] = s.serial.stopBits;
    serial["flow"] = keyOf(s.serial.flow, kFlows);
    serial["dtr"] = s.serial.dataTerminalReady;
    j["serial"] = std::move(serial);

    Json tcp = Json::object();
    tcp["host"] = s.tcp.host;
    tcp["port"] = s.tcp.port;
    j["tcp"] = std::move(tcp);

    Json udp = Json::object();
    udp["port"] = s.udp.port;
    udp["multicastGroup"] = s.udp.multicastGroup;
    udp["bindAddress"] = s.udp.bindAddress;
    j["udp"] = std::move(udp);

    Json scpi = Json::object();
    scpi["transport"] = keyOf(s.scpi.transport, kTransports);
    scpi["host"] = s.scpi.host;
    scpi["port"] = s.scpi.port;
    scpi["visaResource"] = s.scpi.visaResource;
    Json setup = Json::array();
    for (const auto &c : s.scpi.setupCommands) setup.push_back(c);
    scpi["setupCommands"] = std::move(setup);
    scpi["query"] = s.scpi.query;
    scpi["intervalSeconds"] = s.scpi.intervalSeconds;
    scpi["timeoutSeconds"] = s.scpi.timeoutSeconds;
    scpi["checkErrors"] = s.scpi.checkErrors;
    j["scpi"] = std::move(scpi);

    Json audio = Json::object();
    audio["device"] = s.audio.device;
    audio["sampleRate"] = s.audio.sampleRate;
    audio["channels"] = s.audio.channels;
    audio["sampleType"] = keyOf(s.audio.sampleType, kAudioTypes);
    j["audio"] = std::move(audio);

    Json simulated = Json::object();
    Json channels = Json::array();
    for (const SimulatedChannel &c : s.simulated.channels)
    {
        Json cj = Json::object();
        cj["waveform"] = keyOf(c.waveform, kWaveforms);
        cj["amplitude"] = c.amplitude;
        cj["frequencyHz"] = c.frequencyHz;
        cj["phaseDegrees"] = c.phaseDegrees;
        cj["offset"] = c.offset;
        cj["dutyCycle"] = c.dutyCycle;
        cj["noise"] = c.noise;
        channels.push_back(std::move(cj));
    }
    simulated["channels"] = std::move(channels);
    simulated["byteErrorRate"] = s.simulated.byteErrorRate;
    simulated["frameLossRate"] = s.simulated.frameLossRate;
    simulated["rateErrorPpm"] = s.simulated.rateErrorPpm;
    simulated["seed"] = static_cast<double>(s.simulated.seed);
    simulated["textDecimals"] = s.simulated.textDecimals;
    simulated["textLabels"] = s.simulated.textLabels;
    j["simulated"] = std::move(simulated);

    Json decoder = Json::object();
    decoder["kind"] = decoderKey(s.decoder.kind);
    Json text = Json::object();
    text["channelCount"] = s.decoder.text.channelCount;
    text["decimalComma"] = s.decoder.text.decimalComma;
    text["maximumLineLength"] = s.decoder.text.maximumLineLength;
    decoder["text"] = std::move(text);
    Json raw = sampleFormatToJson(s.decoder.raw);
    raw["channels"] = s.decoder.rawChannels;
    decoder["raw"] = std::move(raw);
    decoder["frame"] = frameFormatToJson(s.decoder.frame);
    Json scpiFormat = Json::object();
    scpiFormat["channels"] = s.decoder.scpi.channels;
    scpiFormat["block"] = sampleFormatToJson(s.decoder.scpi.block);
    scpiFormat["maximumAnswerBytes"] = s.decoder.scpi.maximumAnswerBytes;
    decoder["scpi"] = std::move(scpiFormat);
    j["decoder"] = std::move(decoder);

    j["declaredSampleRateHz"] = s.declaredSampleRateHz;
    Json names = Json::array();
    for (const auto &c : s.channels)
    {
        Json cj = Json::object();
        cj["name"] = c.name;
        cj["unit"] = c.unit;
        names.push_back(std::move(cj));
    }
    j["channels"] = std::move(names);
    return j;
}

bool fromJson(const Json &j, AcquisitionSettings &s, std::string *error)
{
    Reader r;
    if (!j.isObject())
    {
        if (error) *error = "Réglages d'acquisition illisibles.";
        return false;
    }
    r.text(j, "name", s.name);
    r.choice(j, "source", s.source, kSources);

    const Json &serial = j["serial"];
    r.text(serial, "port", s.serial.port);
    r.integer(serial, "baudRate", s.serial.baudRate);
    r.integer(serial, "dataBits", s.serial.dataBits);
    r.choice(serial, "parity", s.serial.parity, kParities);
    r.integer(serial, "stopBits", s.serial.stopBits);
    r.choice(serial, "flow", s.serial.flow, kFlows);
    r.boolean(serial, "dtr", s.serial.dataTerminalReady);

    r.text(j["tcp"], "host", s.tcp.host);
    r.integer(j["tcp"], "port", s.tcp.port);
    r.integer(j["udp"], "port", s.udp.port);
    r.text(j["udp"], "multicastGroup", s.udp.multicastGroup);
    r.text(j["udp"], "bindAddress", s.udp.bindAddress);

    const Json &scpi = j["scpi"];
    r.choice(scpi, "transport", s.scpi.transport, kTransports);
    r.text(scpi, "host", s.scpi.host);
    r.integer(scpi, "port", s.scpi.port);
    r.text(scpi, "visaResource", s.scpi.visaResource);
    if (scpi.contains("setupCommands"))
    {
        s.scpi.setupCommands.clear();
        for (const Json &c : scpi["setupCommands"].items()) s.scpi.setupCommands.push_back(c.toString());
    }
    r.text(scpi, "query", s.scpi.query);
    r.number(scpi, "intervalSeconds", s.scpi.intervalSeconds);
    r.number(scpi, "timeoutSeconds", s.scpi.timeoutSeconds);
    r.boolean(scpi, "checkErrors", s.scpi.checkErrors);

    const Json &audio = j["audio"];
    r.text(audio, "device", s.audio.device);
    r.integer(audio, "sampleRate", s.audio.sampleRate);
    r.integer(audio, "channels", s.audio.channels);
    r.choice(audio, "sampleType", s.audio.sampleType, kAudioTypes);

    const Json &simulated = j["simulated"];
    if (simulated.contains("channels"))
    {
        s.simulated.channels.clear();
        for (const Json &cj : simulated["channels"].items())
        {
            SimulatedChannel c;
            r.choice(cj, "waveform", c.waveform, kWaveforms);
            r.number(cj, "amplitude", c.amplitude);
            r.number(cj, "frequencyHz", c.frequencyHz);
            r.number(cj, "phaseDegrees", c.phaseDegrees);
            r.number(cj, "offset", c.offset);
            r.number(cj, "dutyCycle", c.dutyCycle);
            r.number(cj, "noise", c.noise);
            s.simulated.channels.push_back(c);
        }
    }
    r.number(simulated, "byteErrorRate", s.simulated.byteErrorRate);
    r.number(simulated, "frameLossRate", s.simulated.frameLossRate);
    r.number(simulated, "rateErrorPpm", s.simulated.rateErrorPpm);
    std::size_t seed = static_cast<std::size_t>(s.simulated.seed);
    r.size(simulated, "seed", seed);
    s.simulated.seed = seed;
    r.integer(simulated, "textDecimals", s.simulated.textDecimals);
    r.boolean(simulated, "textLabels", s.simulated.textLabels);

    const Json &decoder = j["decoder"];
    r.choice(decoder, "kind", s.decoder.kind, kDecoders);
    r.size(decoder["text"], "channelCount", s.decoder.text.channelCount);
    r.boolean(decoder["text"], "decimalComma", s.decoder.text.decimalComma);
    r.size(decoder["text"], "maximumLineLength", s.decoder.text.maximumLineLength);
    if (decoder.contains("raw"))
    {
        sampleFormatFromJson(decoder["raw"], s.decoder.raw, r);
        r.size(decoder["raw"], "channels", s.decoder.rawChannels);
    }
    if (decoder.contains("frame"))
    {
        std::string why;
        if (!frameFormatFromJson(decoder["frame"], s.decoder.frame, &why) && r.error.empty()) r.error = why;
    }
    if (decoder.contains("scpi"))
    {
        r.size(decoder["scpi"], "channels", s.decoder.scpi.channels);
        if (decoder["scpi"].contains("block")) sampleFormatFromJson(decoder["scpi"]["block"], s.decoder.scpi.block, r);
        r.size(decoder["scpi"], "maximumAnswerBytes", s.decoder.scpi.maximumAnswerBytes);
    }

    r.number(j, "declaredSampleRateHz", s.declaredSampleRateHz);
    if (j.contains("channels"))
    {
        s.channels.clear();
        for (const Json &c : j["channels"].items()) s.channels.push_back({c["name"].toString(), c["unit"].toString()});
    }
    if (error) *error = r.error;
    return r.error.empty();
}

std::string validateSettings(const AcquisitionSettings &s)
{
    auto validPort = [](int p) { return p >= 1 && p <= 65535; };
    switch (s.source)
    {
        case SourceKind::Serial:
            if (s.serial.port.empty()) return "Choisissez un port série.";
            if (s.serial.baudRate < 50 || s.serial.baudRate > 12'000'000) return "Débit série hors limites (50 à 12 000 000 bauds).";
            if (s.serial.dataBits < 5 || s.serial.dataBits > 8) return "Le nombre de bits de données va de 5 à 8.";
            if (s.serial.stopBits != 1 && s.serial.stopBits != 2) return "Un ou deux bits d'arrêt.";
            break;
        case SourceKind::TcpClient:
            if (s.tcp.host.empty()) return "Indiquez l'adresse de l'appareil.";
            if (!validPort(s.tcp.port)) return "Le port TCP va de 1 à 65 535.";
            break;
        case SourceKind::Udp:
            if (!validPort(s.udp.port)) return "Le port UDP va de 1 à 65 535.";
            break;
        case SourceKind::Scpi:
            if (s.scpi.transport == ScpiTransport::Tcp)
            {
                if (s.scpi.host.empty()) return "Indiquez l'adresse de l'instrument.";
                if (!validPort(s.scpi.port)) return "Le port de l'instrument va de 1 à 65 535 (5025 le plus souvent).";
            }
            else if (s.scpi.visaResource.empty())
            {
                return "Indiquez la ressource VISA de l'instrument (USB0::…::INSTR).";
            }
            if (s.scpi.query.empty()) return "Indiquez la requête à envoyer à l'instrument.";
            if (!(s.scpi.intervalSeconds >= 0.0)) return "L'intervalle entre deux requêtes ne peut pas être négatif.";
            if (!(s.scpi.timeoutSeconds > 0.0)) return "Le délai d'attente doit être positif.";
            break;
        case SourceKind::Audio:
            if (s.audio.sampleRate < 1000 || s.audio.sampleRate > 768000) return "Fréquence de la carte son hors limites (1 kHz à 768 kHz).";
            if (s.audio.channels < 1 || s.audio.channels > 32) return "La carte son a de 1 à 32 voies.";
            break;
        case SourceKind::Simulated:
            if (s.simulated.channels.empty() || s.simulated.channels.size() > 64) return "Le simulateur a de 1 à 64 voies.";
            if (!(s.declaredSampleRateHz > 0.0)) return "Indiquez la fréquence d'échantillonnage du simulateur.";
            if (!(s.simulated.byteErrorRate >= 0.0 && s.simulated.byteErrorRate < 1.0) ||
                !(s.simulated.frameLossRate >= 0.0 && s.simulated.frameLossRate < 1.0))
                return "Les taux d'erreur simulés vont de 0 à 1.";
            break;
    }
    if (!(std::isfinite(s.declaredSampleRateHz) && s.declaredSampleRateHz >= 0.0))
        return "La fréquence d'échantillonnage déclarée doit être positive (0 : mesurée).";
    const DecoderSettings d = effectiveDecoder(s);
    switch (d.kind)
    {
        case DecoderKind::Text:
            if (d.text.maximumLineLength < 16) return "Longueur de ligne maximale trop petite.";
            break;
        case DecoderKind::RawBinary:
            if (d.rawChannels == 0 || d.rawChannels > 256) return "Le flux brut a de 1 à 256 voies.";
            if (!(std::isfinite(d.raw.scale) && d.raw.scale != 0.0)) return "Le facteur d'échelle doit être fini et non nul.";
            break;
        case DecoderKind::FramedBinary:
        {
            const std::string why = io::validateFrameFormat(d.frame);
            if (!why.empty()) return why;
            break;
        }
        case DecoderKind::Scpi:
            if (d.scpi.channels == 0 || d.scpi.channels > 256) return "Les réponses SCPI ont de 1 à 256 voies.";
            break;
    }
    if (s.source == SourceKind::Simulated && d.kind == DecoderKind::Text && d.text.channelCount != 0 &&
        d.text.channelCount != s.simulated.channels.size())
        return "Le nombre de voies du format ne correspond pas au simulateur.";
    return {};
}

DecoderSettings effectiveDecoder(const AcquisitionSettings &s)
{
    DecoderSettings d = s.decoder;
    if (s.source == SourceKind::Audio)
    {
        d.kind = DecoderKind::RawBinary;
        d.rawChannels = static_cast<std::size_t>(std::max(1, s.audio.channels));
        d.raw.order = io::ByteOrder::LittleEndian;
        d.raw.offset = 0.0;
        switch (s.audio.sampleType)
        {
            case AudioSampleType::Int16:
                d.raw.type = io::SampleType::Int16;
                d.raw.scale = 1.0 / 32768.0;
                break;
            case AudioSampleType::Int32:
                d.raw.type = io::SampleType::Int32;
                d.raw.scale = 1.0 / 2147483648.0;
                break;
            case AudioSampleType::Float32:
                d.raw.type = io::SampleType::Float32;
                d.raw.scale = 1.0;
                break;
        }
    }
    else if (s.source == SourceKind::Scpi)
    {
        d.kind = DecoderKind::Scpi;
    }
    else if (d.kind == DecoderKind::Scpi)
    {
        d.kind = DecoderKind::Text; // SCPI answers only come from an instrument
    }
    return d;
}

std::unique_ptr<io::Decoder> makeDecoder(const DecoderSettings &d, std::string *error)
{
    switch (d.kind)
    {
        case DecoderKind::Text: return std::make_unique<io::TextDecoder>(d.text);
        case DecoderKind::RawBinary: return std::make_unique<io::RawBinaryDecoder>(d.raw, d.rawChannels);
        case DecoderKind::FramedBinary:
        {
            const std::string why = io::validateFrameFormat(d.frame);
            if (!why.empty())
            {
                if (error) *error = why;
                return nullptr;
            }
            return std::make_unique<io::FramedBinaryDecoder>(d.frame);
        }
        case DecoderKind::Scpi: return std::make_unique<io::ScpiDecoder>(d.scpi);
    }
    return nullptr;
}
} // namespace qacq::acquisition
