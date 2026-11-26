#include "qacq/analysis/frames.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace qacq::analysis
{
namespace
{
constexpr std::array<FrameLineInfo, 2> kBitsLines = {
    {{"data", "Données", FrameLineRole::Data}, {"clock", "Horloge", FrameLineRole::Clock}}};
constexpr std::array<FrameLineInfo, 1> kUartLines = {{{"tx", "TX", FrameLineRole::Data}}};
constexpr std::array<FrameLineInfo, 2> kI2cLines = {
    {{"sda", "SDA", FrameLineRole::Data}, {"scl", "SCL", FrameLineRole::Clock}}};
constexpr std::array<FrameLineInfo, 4> kSpiLines = {{{"mosi", "MOSI", FrameLineRole::Data},
                                                     {"miso", "MISO", FrameLineRole::Second},
                                                     {"sclk", "SCLK", FrameLineRole::Clock},
                                                     {"cs", "CS", FrameLineRole::Select}}};
constexpr std::array<FrameLineInfo, 2> kUsbLines = {
    {{"dp", "D+", FrameLineRole::Data}, {"dm", "D\xE2\x88\x92", FrameLineRole::Second}}};

constexpr std::pair<FrameProtocol, std::string_view> kProtocolKeys[] = {{FrameProtocol::Bits, "bits"},
                                                                       {FrameProtocol::Uart, "uart"},
                                                                       {FrameProtocol::I2c, "i2c"},
                                                                       {FrameProtocol::Spi, "spi"},
                                                                       {FrameProtocol::Usb, "usb"}};
constexpr std::pair<FrameFormat, std::string_view> kFormatKeys[] = {{FrameFormat::Auto, "auto"},
                                                                   {FrameFormat::Binary, "bin"},
                                                                   {FrameFormat::Hex, "hex"},
                                                                   {FrameFormat::Decimal, "dec"},
                                                                   {FrameFormat::Text, "text"}};
constexpr std::pair<LineCode, std::string_view> kCodeKeys[] = {{LineCode::Nrz, "nrz"},
                                                              {LineCode::Nrzi, "nrzi"},
                                                              {LineCode::Manchester, "manchester"},
                                                              {LineCode::Rz, "rz"}};
constexpr std::pair<FrameParity, std::string_view> kParityKeys[] = {{FrameParity::None, "none"},
                                                                   {FrameParity::Even, "even"},
                                                                   {FrameParity::Odd, "odd"}};
constexpr std::pair<I2cTransfer, std::string_view> kTransferKeys[] = {{I2cTransfer::Write, "write"},
                                                                     {I2cTransfer::Read, "read"},
                                                                     {I2cTransfer::WriteRead, "write-read"}};

struct PacketInfo
{
    UsbPacket packet;
    std::string_view key;
    std::string_view pidName; // on the wire
    unsigned pid;             // the 4 bits of the PID and their complement above
    std::string_view title;   // in a sentence
};
constexpr PacketInfo kPackets[] = {{UsbPacket::Setup, "setup", "SETUP", 0x2D, "jeton SETUP"},
                                   {UsbPacket::Out, "out", "OUT", 0xE1, "jeton OUT"},
                                   {UsbPacket::In, "in", "IN", 0x69, "jeton IN"},
                                   {UsbPacket::Sof, "sof", "SOF", 0xA5, "début de trame SOF"},
                                   {UsbPacket::Data0, "data0", "DATA0", 0xC3, "paquet DATA0"},
                                   {UsbPacket::Data1, "data1", "DATA1", 0x4B, "paquet DATA1"},
                                   {UsbPacket::Ack, "ack", "ACK", 0xD2, "acquittement ACK"},
                                   {UsbPacket::Nak, "nak", "NAK", 0x5A, "refus NAK"},
                                   {UsbPacket::Stall, "stall", "STALL", 0x1E, "blocage STALL"},
                                   {UsbPacket::SetupTransaction, "setup-transaction", "SETUP", 0x2D, "transaction SETUP"}};

const PacketInfo &packetInfo(UsbPacket packet) noexcept
{
    for (const PacketInfo &info : kPackets)
        if (info.packet == packet) return info;
    return kPackets[0];
}

template <typename Enum, std::size_t N>
std::string_view keyOf(const std::pair<Enum, std::string_view> (&keys)[N], Enum value) noexcept
{
    for (const auto &[v, key] : keys)
        if (v == value) return key;
    return keys[0].second;
}

template <typename Enum, std::size_t N>
Enum fromKey(const std::pair<Enum, std::string_view> (&keys)[N], std::string_view key, Enum fallback) noexcept
{
    for (const auto &[v, k] : keys)
        if (k == key) return v;
    return fallback;
}

bool isBlank(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trimmed(std::string_view s) noexcept
{
    while (!s.empty() && isBlank(s.front())) s.remove_prefix(1);
    while (!s.empty() && isBlank(s.back())) s.remove_suffix(1);
    return s;
}

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char lowered(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The character (a whole UTF-8 sequence) at `at`, to name it in a message.
std::string glyphAt(std::string_view s, std::size_t at)
{
    if (at >= s.size()) return {};
    const auto lead = static_cast<unsigned char>(s[at]);
    std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    length = std::min(length, s.size() - at);
    return std::string(s.substr(at, length));
}

std::string grouped(std::size_t value)
{
    std::string digits = std::to_string(value);
    for (std::size_t i = digits.size(); i > 3; i -= 3) digits.insert(i - 3, " ");
    return digits;
}

std::string hex2(unsigned value)
{
    constexpr char kDigits[] = "0123456789ABCDEF";
    return {kDigits[(value >> 4) & 0xF], kDigits[value & 0xF]};
}

std::string hexByte(unsigned value)
{
    return "0x" + hex2(value);
}

// "0x42 « B »", "0x0A".
std::string byteNote(unsigned value)
{
    std::string text = hexByte(value);
    if (value >= 0x21 && value <= 0x7E) text += " « " + std::string(1, static_cast<char>(value)) + " »";
    if (value == 0x20) text += " (espace)";
    return text;
}

std::string counted(std::size_t count, std::string_view one, std::string_view many)
{
    return grouped(count) + " " + std::string(count > 1 ? many : one);
}

// A space as typed before a colon or inside quotation marks: a plain one, a tab, a no-break one (U+00A0,
// U+202F). Its length in bytes, 0 if there is none at `at`.
std::size_t spaceAt(std::string_view s, std::size_t at) noexcept
{
    if (at >= s.size()) return 0;
    if (s[at] == ' ' || s[at] == '\t') return 1;
    if (s.substr(at).starts_with("\xC2\xA0")) return 2;
    if (s.substr(at).starts_with("\xE2\x80\xAF")) return 3;
    return 0;
}

// "ascii : …", "hex: …", "bin: …", "dec: …": the format said in front of the data.
bool takePrefix(std::string_view &text, FrameFormat &format) noexcept
{
    constexpr std::pair<std::string_view, FrameFormat> kWords[] = {
        {"ascii", FrameFormat::Text},        {"texte", FrameFormat::Text},         {"text", FrameFormat::Text},
        {"txt", FrameFormat::Text},          {"utf-8", FrameFormat::Text},         {"utf8", FrameFormat::Text},
        {"hexadécimal", FrameFormat::Hex},   {"hexadecimal", FrameFormat::Hex},    {"hexa", FrameFormat::Hex},
        {"hex", FrameFormat::Hex},           {"binaire", FrameFormat::Binary},     {"binary", FrameFormat::Binary},
        {"bits", FrameFormat::Binary},       {"bin", FrameFormat::Binary},         {"décimal", FrameFormat::Decimal},
        {"decimal", FrameFormat::Decimal},   {"dec", FrameFormat::Decimal}};
    for (const auto &[word, f] : kWords)
    {
        if (text.size() <= word.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < word.size() && same; ++i) same = lowered(text[i]) == word[i];
        if (!same) continue;
        std::size_t at = word.size();
        for (std::size_t step = spaceAt(text, at); step > 0; step = spaceAt(text, at)) at += step;
        if (at >= text.size() || text[at] != ':') continue;
        text = trimmed(text.substr(at + 1));
        format = f;
        return true;
    }
    return false;
}

// The text without the quotation marks around it ("…", '…', «…», “…”, ‘…’), if it has them.
std::string_view unquoted(std::string_view t) noexcept
{
    constexpr std::pair<std::string_view, std::string_view> kPairs[] = {{"\"", "\""},
                                                                        {"'", "'"},
                                                                        {"\xC2\xAB", "\xC2\xBB"},
                                                                        {"\xE2\x80\x9C", "\xE2\x80\x9D"},
                                                                        {"\xE2\x80\x98", "\xE2\x80\x99"}};
    for (const auto &[open, close] : kPairs)
    {
        if (t.size() < open.size() + close.size() || !t.starts_with(open) || !t.ends_with(close)) continue;
        std::string_view inner = t.substr(open.size(), t.size() - open.size() - close.size());
        if (open == "\xC2\xAB")
        {
            // « bonjour »: the spaces inside French quotation marks are not part of the text.
            inner.remove_prefix(spaceAt(inner, 0));
            for (const std::string_view space : {std::string_view(" "), std::string_view("\xC2\xA0"), std::string_view("\xE2\x80\xAF")})
            {
                if (!inner.ends_with(space)) continue;
                inner.remove_suffix(space.size());
                break;
            }
        }
        return inner;
    }
    return t;
}

bool onlyBinary(std::string_view t) noexcept
{
    bool digit = false;
    for (const char c : t)
    {
        if (c == '0' || c == '1')
            digit = true;
        else if (!isBlank(c) && c != '_')
            return false;
    }
    return digit;
}

void addByte(FrameData &d, unsigned value)
{
    for (int b = 7; b >= 0; --b) d.bits.push_back(static_cast<std::uint8_t>((value >> b) & 1u));
    d.groups.push_back(8);
}

// The pieces of a text between separators (blanks, and those of `separators`).
template <typename Function>
bool eachToken(std::string_view text, std::string_view separators, Function &&function)
{
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto separates = [separators](char c) { return isBlank(c) || separators.find(c) != std::string_view::npos; };
        while (at < text.size() && separates(text[at])) ++at;
        std::size_t end = at;
        while (end < text.size() && !separates(text[end])) ++end;
        if (end > at && !function(text.substr(at, end - at))) return false;
        at = end;
    }
    return true;
}

void parseBinary(std::string_view text, FrameData &d)
{
    eachToken(text, ",;", [&d](std::string_view token) {
        if (token.starts_with("0b") || token.starts_with("0B")) token.remove_prefix(2);
        std::size_t count = 0;
        for (std::size_t i = 0; i < token.size(); ++i)
        {
            const char c = token[i];
            if (c == '_' || c == '\'') continue;
            if (c != '0' && c != '1')
            {
                d.error = "« " + glyphAt(token, i) + " » n'est pas un chiffre binaire (0 ou 1) ; pour du texte ou de l'hexadécimal, "
                          "changez le format.";
                return false;
            }
            d.bits.push_back(static_cast<std::uint8_t>(c - '0'));
            ++count;
        }
        if (count > 0) d.groups.push_back(count);
        return true;
    });
}

void parseHex(std::string_view text, FrameData &d)
{
    eachToken(text, ",;:-", [&d](std::string_view token) {
        if (token.starts_with("0x") || token.starts_with("0X")) token.remove_prefix(2);
        if (token.size() > 1 && (token.back() == 'h' || token.back() == 'H')) token.remove_suffix(1);
        std::vector<int> digits;
        for (std::size_t i = 0; i < token.size(); ++i)
        {
            if (token[i] == '_') continue;
            const int v = hexValue(token[i]);
            if (v < 0)
            {
                d.error = "« " + glyphAt(token, i) + " » n'est pas un chiffre hexadécimal (0 à 9, A à F).";
                return false;
            }
            digits.push_back(v);
        }
        if (digits.size() % 2 == 1) digits.insert(digits.begin(), 0);
        for (std::size_t i = 0; i + 1 < digits.size(); i += 2) addByte(d, static_cast<unsigned>(digits[i] * 16 + digits[i + 1]));
        return true;
    });
}

void parseDecimal(std::string_view text, FrameData &d)
{
    eachToken(text, ",;", [&d](std::string_view token) {
        unsigned value = 0;
        bool valid = token.size() <= 3;
        for (std::size_t i = 0; i < token.size() && valid; ++i)
        {
            valid = token[i] >= '0' && token[i] <= '9';
            value = value * 10 + static_cast<unsigned>(token[i] - '0');
        }
        if (!valid || value > 255)
        {
            d.error = "« " + std::string(token.substr(0, 16)) + " » n'est pas un nombre de 0 à 255 (un octet par nombre).";
            return false;
        }
        addByte(d, value);
        return true;
    });
}

void parseText(std::string_view text, FrameData &d)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        auto value = static_cast<unsigned char>(text[i]);
        if (text[i] == '\\' && i + 1 < text.size())
        {
            const char next = text[i + 1];
            bool escaped = true;
            std::size_t taken = 1;
            switch (next)
            {
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case '0': value = 0; break;
                case '\\': value = '\\'; break;
                case '"': value = '"'; break;
                case '\'': value = '\''; break;
                case 'x':
                    if (i + 3 < text.size() && hexValue(text[i + 2]) >= 0 && hexValue(text[i + 3]) >= 0)
                    {
                        value = static_cast<unsigned char>(hexValue(text[i + 2]) * 16 + hexValue(text[i + 3]));
                        taken = 3;
                    }
                    else
                    {
                        escaped = false;
                    }
                    break;
                default: escaped = false; break;
            }
            if (escaped) i += taken;
        }
        addByte(d, value);
    }
}

constexpr std::string_view kNoData =
    "Aucune donnée à envoyer : tapez des bits (0011 0010), de l'hexadécimal (0x2831) ou du texte (Bonjour).";
constexpr std::uint8_t kLow = 0, kHigh = 1;

std::string needsBytes(std::string_view who, const FrameData &d)
{
    return std::string(who) + " envoie des octets : tapez les bits par huit (" + counted(d.bits.size(), "bit tapé", "bits tapés") + ").";
}

// " 42 6F 6E 6A": the first bytes in hexadecimal, each after a space; " …" when there are more.
std::string hexList(const std::vector<std::uint8_t> &bytes)
{
    constexpr std::size_t kShown = 12;
    std::string text;
    for (std::size_t i = 0; i < bytes.size() && i < kShown; ++i) text += " " + hex2(bytes[i]);
    if (bytes.size() > kShown) text += " \xE2\x80\xA6";
    return text;
}

// "7 octets : 42 6F 6E 6A 6F 75 72", "11 bits".
std::string dataSummary(const FrameData &d, std::string_view one = "octet", std::string_view many = "octets")
{
    if (d.bits.empty()) return "aucune donnée";
    if (!d.wholeBytes()) return counted(d.bits.size(), "bit", "bits");
    return counted(d.bytes.size(), one, many) + " :" + hexList(d.bytes);
}

BuiltFrame failed(std::string error)
{
    BuiltFrame frame;
    frame.error = std::move(error);
    return frame;
}

class Builder
{
public:
    Builder(std::initializer_list<std::uint8_t> idle, std::size_t ticksPerBit)
    {
        m_frame.idle.assign(idle);
        m_frame.levels.resize(idle.size());
        m_frame.ticksPerBit = ticksPerBit;
    }
    std::size_t tick() const noexcept { return m_frame.levels.front().size(); }
    // One tick more: the level of each line.
    void push(std::initializer_list<std::uint8_t> levels)
    {
        std::size_t line = 0;
        for (const std::uint8_t v : levels) m_frame.levels[line++].push_back(v);
    }
    // A field from that tick to the last one pushed.
    FrameField &field(std::string text, std::string note, std::size_t firstTick, bool startsGroup = false)
    {
        FrameField f;
        f.text = std::move(text);
        f.note = std::move(note);
        f.firstTick = firstTick;
        f.tickCount = tick() - firstTick;
        f.startsGroup = startsGroup;
        m_frame.fields.push_back(std::move(f));
        return m_frame.fields.back();
    }
    void warn(std::string text) { m_frame.warnings.push_back(std::move(text)); }
    BuiltFrame take(std::string legend, std::string summary, const FrameData &data)
    {
        m_frame.tickCount = tick();
        m_frame.legend = std::move(legend);
        m_frame.summary = std::move(summary);
        m_frame.byteCount = data.wholeBytes() ? data.bytes.size() : 0;
        m_frame.format = data.format;
        return std::move(m_frame);
    }

private:
    BuiltFrame m_frame;
};

BuiltFrame buildBits(const FrameSpec &spec, const FrameData &data)
{
    if (data.bits.empty()) return failed(std::string(kNoData));
    Builder b({0, 0}, 2);
    std::uint8_t level = 0; // of an NRZI line
    std::size_t at = 0;
    for (const std::size_t size : data.groups)
    {
        const std::span<const std::uint8_t> group(data.bits.data() + at, size);
        at += size;
        const std::size_t first = b.tick();
        std::string text;
        unsigned value = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            value = (value << 1) | group[i];
            const std::uint8_t bit = group[spec.lsbFirst ? size - 1 - i : i];
            text += bit ? '1' : '0';
            std::uint8_t firstHalf = bit, secondHalf = bit;
            switch (spec.code)
            {
                case LineCode::Nrz: break;
                case LineCode::Nrzi:
                    if (bit) level = static_cast<std::uint8_t>(level ^ 1u);
                    firstHalf = secondHalf = level;
                    break;
                case LineCode::Manchester: firstHalf = static_cast<std::uint8_t>(bit ^ 1u); break;
                case LineCode::Rz: secondHalf = 0; break;
            }
            b.push({firstHalf, 0});
            b.push({secondHalf, 1});
        }
        b.field(std::move(text), size == 8 ? byteNote(value) : counted(size, "bit", "bits"), first, true);
    }
    std::string legend;
    switch (spec.code)
    {
        case LineCode::Nrz: legend = "NRZ : le niveau est le bit."; break;
        case LineCode::Nrzi: legend = "NRZI : le niveau change à chaque 1, reste pour un 0."; break;
        case LineCode::Manchester:
            legend = "Manchester (IEEE 802.3) : un front montant au milieu du bit pour un 1, descendant pour un 0.";
            break;
        case LineCode::Rz: legend = "RZ : un 1 est haut pendant la première moitié du bit, un 0 reste bas."; break;
    }
    legend += spec.lsbFirst ? " Chaque groupe part de son bit de droite (poids faible d'abord)."
                            : " Chaque groupe part de son bit de gauche (poids fort d'abord).";
    legend += " L'horloge monte au milieu de chaque bit.";
    return b.take(std::move(legend), dataSummary(data), data);
}

BuiltFrame buildUart(const FrameSpec &spec, const FrameData &data)
{
    if (data.bits.empty()) return failed(std::string(kNoData));
    if (!data.wholeBytes()) return failed(needsBytes("L'UART", data));
    const int width = std::clamp(spec.dataBits, 5, 8);
    const int stopTicks = spec.stopBits >= 1.75 ? 4 : spec.stopBits >= 1.25 ? 3 : 2;
    Builder b({1}, 2);
    std::size_t tooWide = 0;
    unsigned firstWide = 0;
    for (const std::uint8_t byte : data.bytes)
    {
        std::size_t first = b.tick();
        b.push({0});
        b.push({0});
        b.field("St", "Bit de départ (0)", first, true);

        first = b.tick();
        const unsigned kept = byte & ((1u << width) - 1u);
        if (kept != byte && tooWide++ == 0) firstWide = byte;
        std::string text;
        unsigned ones = 0;
        for (int i = 0; i < width; ++i)
        {
            const auto bit = static_cast<std::uint8_t>((kept >> (spec.lsbFirst ? i : width - 1 - i)) & 1u);
            ones += bit;
            text += bit ? '1' : '0';
            b.push({bit});
            b.push({bit});
        }
        b.field(std::move(text), byteNote(byte), first);

        if (spec.parity != FrameParity::None)
        {
            first = b.tick();
            const bool even = spec.parity == FrameParity::Even;
            const auto parity = static_cast<std::uint8_t>(even ? ones % 2 : 1 - ones % 2);
            b.push({parity});
            b.push({parity});
            b.field(parity ? "P1" : "P0", even ? "Parité paire : un nombre pair de 1 avec les données" : "Parité impaire : un nombre impair de 1 avec les données",
                    first);
        }

        first = b.tick();
        for (int i = 0; i < stopTicks; ++i) b.push({1});
        b.field("Sp", stopTicks == 2 ? "Bit d'arrêt (1)" : stopTicks == 3 ? "Arrêt : 1,5 bit à 1" : "Arrêt : 2 bits à 1", first);
    }
    if (tooWide > 0)
        b.warn(hexByte(firstWide) + (tooWide > 1 ? " et " + counted(tooWide - 1, "autre octet", "autres octets") + " ne tiennent" : " ne tient") +
               " pas sur " + std::to_string(width) + " bits : les bits de poids fort ne sont pas envoyés.");
    std::string legend = "St : départ (0) · ";
    if (spec.parity != FrameParity::None) legend += "P : parité · ";
    legend += std::string("Sp : arrêt (1) · données dans l'ordre d'envoi, ") + (spec.lsbFirst ? "poids faible d'abord." : "poids fort d'abord.");
    return b.take(std::move(legend), dataSummary(data), data);
}

BuiltFrame buildI2c(const FrameSpec &spec, const FrameData &data, const FrameData &reply)
{
    const bool reads = spec.i2cTransfer == I2cTransfer::Read;
    const bool both = spec.i2cTransfer == I2cTransfer::WriteRead;
    if (!data.wholeBytes()) return failed(needsBytes("L'I2C", data));
    if (!spec.i2cNoAnswer)
    {
        if (reads && data.bytes.empty()) return failed("Tapez les octets que l'esclave renvoie : une lecture en lit au moins un.");
        if (both && data.bytes.empty())
            return failed("Tapez les octets écrits avant la lecture (le numéro d'un registre, par exemple).");
        if (both && !reply.wholeBytes()) return failed("Réponse : " + needsBytes("l'I2C", reply));
        if (both && reply.bytes.empty()) return failed("Réponse : tapez les octets lus après le redémarrage.");
    }
    const unsigned address = spec.i2cAddress & 0x7Fu;

    Builder b({1, 1}, 4);
    std::uint8_t sda = 1;
    // One bit: SCL low then high, SDA changing in the middle of SCL low.
    const auto cell = [&b, &sda](std::uint8_t bit) {
        b.push({sda, 0});
        b.push({bit, 0});
        b.push({bit, 1});
        b.push({bit, 1});
        sda = bit;
    };
    const auto sendAddress = [&](bool reading) {
        std::size_t first = b.tick();
        std::string text;
        for (int i = 6; i >= 0; --i)
        {
            const auto bit = static_cast<std::uint8_t>((address >> i) & 1u);
            text += bit ? '1' : '0';
            cell(bit);
        }
        b.field(std::move(text), "Adresse " + hexByte(address) + " (7 bits)", first);
        first = b.tick();
        cell(reading ? kHigh : kLow);
        b.field(reading ? "R" : "W", reading ? "Lecture (1)" : "Écriture (0)", first);
        first = b.tick();
        cell(spec.i2cNoAnswer ? kHigh : kLow);
        b.field(spec.i2cNoAnswer ? "N" : "A", spec.i2cNoAnswer ? "Personne ne répond : SDA reste à 1" : "Acquittement de l'esclave (0)", first);
    };
    const auto sendBytes = [&](const std::vector<std::uint8_t> &bytes, bool reading) {
        for (std::size_t k = 0; k < bytes.size(); ++k)
        {
            std::size_t first = b.tick();
            std::string text;
            for (int i = 7; i >= 0; --i)
            {
                const auto bit = static_cast<std::uint8_t>((bytes[k] >> i) & 1u);
                text += bit ? '1' : '0';
                cell(bit);
            }
            b.field(std::move(text), byteNote(bytes[k]) + (reading ? ", envoyé par l'esclave" : ""), first, true);
            first = b.tick();
            const bool last = reading && k + 1 == bytes.size();
            cell(last ? kHigh : kLow);
            b.field(last ? "N" : "A",
                    !reading ? "Acquittement de l'esclave (0)"
                    : last   ? "Non-acquittement du maître (1) : c'était le dernier octet"
                             : "Acquittement du maître (0)",
                    first);
        }
    };

    std::size_t first = b.tick();
    b.push({1, 1});
    b.push({1, 1});
    b.push({0, 1});
    b.push({0, 1});
    sda = 0;
    b.field("S", "Départ : SDA descend pendant que SCL est haut", first, true);
    sendAddress(reads);
    if (!spec.i2cNoAnswer)
    {
        sendBytes(data.bytes, reads);
        if (both)
        {
            first = b.tick();
            b.push({sda, 0});
            b.push({1, 0});
            b.push({1, 1});
            b.push({0, 1});
            sda = 0;
            b.field("Sr", "Redémarrage : un nouveau départ, sans arrêt avant", first, true);
            sendAddress(true);
            sendBytes(reply.bytes, true);
        }
    }
    first = b.tick();
    b.push({sda, 0});
    b.push({0, 0});
    b.push({0, 1});
    b.push({1, 1});
    b.field("P", "Arrêt : SDA monte pendant que SCL est haut", first, true);

    std::string summary;
    if (spec.i2cNoAnswer)
        summary = "adresse sans réponse";
    else if (both)
        summary = dataSummary(data, "octet écrit", "octets écrits") + " ; " + dataSummary(reply, "octet lu", "octets lus");
    else if (data.bytes.empty())
        summary = "adresse seule";
    else
        summary = reads ? dataSummary(data, "octet lu", "octets lus") : dataSummary(data, "octet écrit", "octets écrits");
    std::string legend = std::string("S : départ · ") + (reads ? "R : lecture (1)" : "W : écriture (0)") +
                         (both ? " · Sr : redémarrage · R : lecture (1)" : "") +
                         " · A : acquittement (0) · N : non-acquittement (1) · P : arrêt · octets poids fort d'abord.";
    return b.take(std::move(legend), std::move(summary), data);
}

BuiltFrame buildSpi(const FrameSpec &spec, const FrameData &data, const FrameData &reply)
{
    if (data.bits.empty() && reply.bits.empty()) return failed(std::string(kNoData));
    if (!data.wholeBytes()) return failed(needsBytes("Le SPI", data));
    if (!reply.wholeBytes()) return failed("Réponse : " + needsBytes("le SPI", reply));
    const int mode = std::clamp(spec.spiMode, 0, 3);
    const auto rest = static_cast<std::uint8_t>(mode >= 2 ? 1 : 0); // of the clock (CPOL)
    const auto active = static_cast<std::uint8_t>(rest ^ 1u);
    const bool second = mode % 2 == 1; // data taken on the second edge of each bit (CPHA)
    const std::size_t count = std::max(data.bytes.size(), reply.bytes.size());
    const bool lsb = spec.lsbFirst;
    const auto bitOf = [lsb](const std::vector<std::uint8_t> &bytes, std::size_t k, int i) {
        return k < bytes.size() ? static_cast<std::uint8_t>((bytes[k] >> (lsb ? i : 7 - i)) & 1u) : std::uint8_t{0};
    };

    Builder b({0, 0, rest, 1}, 2);
    // CS falls half a bit before the first bit; data taken on the first edge are already there.
    std::uint8_t out = second ? std::uint8_t{0} : bitOf(data.bytes, 0, 0);
    std::uint8_t in = second ? std::uint8_t{0} : bitOf(reply.bytes, 0, 0);
    b.push({out, in, rest, 0});
    for (std::size_t k = 0; k < count; ++k)
    {
        const std::size_t first = b.tick();
        std::string sent, received;
        for (int i = 0; i < 8; ++i)
        {
            out = bitOf(data.bytes, k, i);
            in = bitOf(reply.bytes, k, i);
            sent += out ? '1' : '0';
            received += in ? '1' : '0';
            b.push({out, in, second ? active : rest, 0});
            b.push({out, in, second ? rest : active, 0});
        }
        std::string note = k < data.bytes.size() ? "MOSI " + byteNote(data.bytes[k]) : std::string("MOSI 0x00 (rien à envoyer)");
        if (!reply.bytes.empty())
            note += k < reply.bytes.size() ? " · MISO " + byteNote(reply.bytes[k]) : std::string(" · MISO 0x00 (réponse finie)");
        FrameField &field = b.field(std::move(sent), std::move(note), first, true);
        if (!reply.bytes.empty()) field.reply = std::move(received);
    }
    b.push({out, in, rest, 0});

    std::string summary = dataSummary(data);
    if (!reply.bytes.empty())
        summary = counted(count, "octet échangé", "octets échangés") + " : MOSI" + (data.bytes.empty() ? std::string(" (des 0)") : hexList(data.bytes)) +
                  " ; MISO" + hexList(reply.bytes);
    constexpr std::string_view kModes[] = {
        "horloge au repos à 0, données lues sur le front montant (le premier)",
        "horloge au repos à 0, données lues sur le front descendant (le second)",
        "horloge au repos à 1, données lues sur le front descendant (le premier)",
        "horloge au repos à 1, données lues sur le front montant (le second)"};
    std::string legend = "Mode " + std::to_string(mode) + " : " + std::string(kModes[mode]) + " · CS à 0 pendant la trame · octets " +
                         (lsb ? "poids faible d'abord." : "poids fort d'abord.");
    return b.take(std::move(legend), std::move(summary), data);
}

// One USB packet after another: bit stuffing, NRZI, end of packet.
class UsbWriter
{
public:
    UsbWriter(Builder &builder, bool fullSpeed)
        : m_builder(builder), m_high(static_cast<std::uint8_t>(fullSpeed ? 1 : 0)), m_low(static_cast<std::uint8_t>(fullSpeed ? 0 : 1))
    {
    }
    // The bits of `value` from its bit 0, as a field.
    void send(const std::string &label, unsigned value, int bitCount, std::string note, bool startsGroup = true)
    {
        const std::size_t first = m_builder.tick();
        std::string text = label.empty() ? std::string() : label + " ";
        for (int i = 0; i < bitCount; ++i)
        {
            const bool bit = ((value >> i) & 1u) != 0;
            text += bit ? '1' : '0';
            if (bit)
            {
                ++m_ones;
            }
            else
            {
                m_j = !m_j;
                m_ones = 0;
            }
            state();
            if (m_ones == 6)
            {
                // Six ones in a row: a zero is stuffed (a change of state the receiver stays locked on).
                m_j = !m_j;
                m_ones = 0;
                state();
                text += "[0]";
                ++m_stuffed;
            }
        }
        FrameField &field = m_builder.field(std::move(text), std::move(note), first, startsGroup);
        field.startsLine = m_newLine;
        m_newLine = false;
    }
    void sync() { send("SYNC", 0x80, 8, "Synchronisation : sept 0 puis un 1 (KJKJKJKK)"); }
    void pid(const PacketInfo &info)
    {
        const unsigned low = info.pid & 0xFu;
        std::string bits;
        for (int i = 0; i < 4; ++i) bits += ((low >> i) & 1u) ? '1' : '0';
        send("PID", info.pid, 8, std::string(info.pidName) + " (" + hexByte(info.pid) + ") : " + bits + " puis son complément");
    }
    void endOfPacket()
    {
        const std::size_t first = m_builder.tick();
        m_builder.push({0, 0});
        m_builder.push({0, 0});
        m_builder.push({m_high, m_low});
        m_builder.field("EOP", "Fin de paquet : les deux lignes à 0 pendant deux bits (SE0), puis J", first, true);
        m_j = true;
        m_ones = 0;
    }
    // Idle (J) between two packets of a transaction; the next one is shown on a new line.
    void turnAround()
    {
        for (int i = 0; i < 3; ++i) m_builder.push({m_high, m_low});
        m_newLine = true;
    }
    std::size_t stuffed() const noexcept { return m_stuffed; }

private:
    void state()
    {
        if (m_j)
            m_builder.push({m_high, m_low});
        else
            m_builder.push({m_low, m_high});
    }

    Builder &m_builder;
    std::uint8_t m_high, m_low; // D+ and D− in the state J
    bool m_j = true;
    int m_ones = 0;
    std::size_t m_stuffed = 0;
    bool m_newLine = false;
};

void usbToken(UsbWriter &w, const PacketInfo &info, unsigned address, unsigned endpoint)
{
    w.sync();
    w.pid(info);
    w.send("ADDR", address, 7, "Adresse " + std::to_string(address));
    w.send("ENDP", endpoint, 4, "Point de terminaison " + std::to_string(endpoint));
    const unsigned crc = usbCrc5(address | (endpoint << 7), 11);
    w.send("CRC5", crc, 5, "CRC5 de l'adresse et du point de terminaison : " + hexByte(crc));
    w.endOfPacket();
}

void usbData(UsbWriter &w, const PacketInfo &info, const std::vector<std::uint8_t> &bytes)
{
    w.sync();
    w.pid(info);
    for (std::size_t k = 0; k < bytes.size(); ++k) w.send(k == 0 ? "DATA" : "", bytes[k], 8, byteNote(bytes[k]), k == 0);
    const unsigned crc = usbCrc16(bytes);
    w.send("CRC16", crc, 16, "CRC16 des données : 0x" + hex2(crc >> 8) + hex2(crc & 0xFFu));
    w.endOfPacket();
}

void usbHandshake(UsbWriter &w, const PacketInfo &info)
{
    w.sync();
    w.pid(info);
    w.endOfPacket();
}

BuiltFrame buildUsb(const FrameSpec &spec, const FrameData &data)
{
    const PacketInfo &info = packetInfo(spec.usbPacket);
    const bool carries = spec.usbPacket == UsbPacket::Data0 || spec.usbPacket == UsbPacket::Data1 ||
                         spec.usbPacket == UsbPacket::SetupTransaction;
    if (carries && !data.wholeBytes()) return failed(needsBytes("L'USB", data));
    if (spec.usbPacket == UsbPacket::SetupTransaction && data.bytes.empty())
        return failed("Tapez les 8 octets de la requête : 0x80 0x06 0x00 0x01 0x00 0x00 0x12 0x00 demande le descripteur de l'appareil.");
    const unsigned address = spec.usbAddress & 0x7Fu, endpoint = spec.usbEndpoint & 0xFu, number = spec.usbFrameNumber & 0x7FFu;

    const auto high = static_cast<std::uint8_t>(spec.usbFullSpeed ? 1 : 0);
    const auto low = static_cast<std::uint8_t>(spec.usbFullSpeed ? 0 : 1);
    Builder b({high, low}, 1);
    UsbWriter w(b, spec.usbFullSpeed);
    std::string summary;
    switch (spec.usbPacket)
    {
        case UsbPacket::Setup:
        case UsbPacket::Out:
        case UsbPacket::In:
            usbToken(w, info, address, endpoint);
            summary = "adresse " + std::to_string(address) + ", point de terminaison " + std::to_string(endpoint);
            break;
        case UsbPacket::Sof:
        {
            w.sync();
            w.pid(info);
            w.send("FRAME", number, 11, "Numéro de trame " + std::to_string(number));
            const unsigned crc = usbCrc5(number, 11);
            w.send("CRC5", crc, 5, "CRC5 du numéro de trame : " + hexByte(crc));
            w.endOfPacket();
            summary = "trame n° " + std::to_string(number);
            if (!spec.usbFullSpeed)
                b.warn("Le bas débit ne reçoit pas de paquet SOF : un simple EOP chaque milliseconde le remplace.");
            break;
        }
        case UsbPacket::Data0:
        case UsbPacket::Data1:
            usbData(w, info, data.bytes);
            summary = dataSummary(data);
            break;
        case UsbPacket::Ack:
        case UsbPacket::Nak:
        case UsbPacket::Stall:
            usbHandshake(w, info);
            summary = "le PID seul";
            break;
        case UsbPacket::SetupTransaction:
            usbToken(w, info, address, endpoint);
            w.turnAround();
            usbData(w, packetInfo(UsbPacket::Data0), data.bytes);
            w.turnAround();
            usbHandshake(w, packetInfo(UsbPacket::Ack));
            summary = "adresse " + std::to_string(address) + ", point de terminaison " + std::to_string(endpoint) + " ; " + dataSummary(data);
            if (data.bytes.size() != 8)
                b.warn("Une requête SETUP compte 8 octets (" + std::to_string(data.bytes.size()) + " ici).");
            break;
    }
    if (carries && spec.usbPacket != UsbPacket::SetupTransaction)
    {
        const std::size_t most = spec.usbFullSpeed ? 1023 : 8;
        if (data.bytes.size() > most)
            b.warn(std::string(spec.usbFullSpeed ? "En plein débit" : "En bas débit") + ", un paquet de données porte au plus " + grouped(most) +
                   " octets (" + grouped(data.bytes.size()) + " ici).");
    }
    std::string legend = "Bits dans l'ordre d'envoi, poids faible d'abord · ";
    if (w.stuffed() > 0) legend += "[0] : bit de bourrage après six 1 · ";
    legend += std::string("EOP : fin de paquet · sur les lignes, un 0 change l'état (NRZI) ; J : ") + (spec.usbFullSpeed ? "D+" : "D\xE2\x88\x92") + " haut.";
    return b.take(std::move(legend), std::move(summary), data);
}
} // namespace

std::span<const FrameLineInfo> frameLines(FrameProtocol protocol) noexcept
{
    switch (protocol)
    {
        case FrameProtocol::Bits: return kBitsLines;
        case FrameProtocol::Uart: return kUartLines;
        case FrameProtocol::I2c: return kI2cLines;
        case FrameProtocol::Spi: return kSpiLines;
        case FrameProtocol::Usb: return kUsbLines;
    }
    return kBitsLines;
}

std::size_t frameLineIndex(FrameProtocol protocol, std::string_view key) noexcept
{
    const std::span<const FrameLineInfo> lines = frameLines(protocol);
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].key == key) return i;
    return 0;
}

std::string_view frameLineFor(FrameProtocol from, std::string_view key, FrameProtocol to) noexcept
{
    const FrameLineRole role = frameLines(from)[frameLineIndex(from, key)].role;
    const std::span<const FrameLineInfo> lines = frameLines(to);
    for (const FrameLineInfo &line : lines)
        if (line.role == role) return line.key;
    return lines.front().key;
}

FrameData parseFrameData(std::string_view text, FrameFormat format)
{
    FrameData d;
    std::string_view body = format == FrameFormat::Text ? text : trimmed(text);
    if (format == FrameFormat::Auto)
    {
        const std::string_view bare = unquoted(body);
        if (takePrefix(body, format))
        {
            if (format == FrameFormat::Text) body = unquoted(body);
        }
        else if (bare.size() != body.size())
        {
            format = FrameFormat::Text;
            body = bare;
        }
        else if (body.starts_with("0x") || body.starts_with("0X"))
            format = FrameFormat::Hex;
        else if (body.starts_with("0b") || body.starts_with("0B") || onlyBinary(body))
            format = FrameFormat::Binary;
        else
            format = FrameFormat::Text;
    }
    d.format = format;
    switch (format)
    {
        case FrameFormat::Auto:
        case FrameFormat::Text: parseText(body, d); break;
        case FrameFormat::Binary: parseBinary(body, d); break;
        case FrameFormat::Hex: parseHex(body, d); break;
        case FrameFormat::Decimal: parseDecimal(body, d); break;
    }
    if (d.ok() && d.bits.size() > 8 * kMaximumFrameBytes)
        d.error = "au plus " + grouped(kMaximumFrameBytes) + " octets (" + grouped((d.bits.size() + 7) / 8) + " tapés).";
    if (!d.ok())
    {
        d.bits.clear();
        d.groups.clear();
        return d;
    }
    if (d.wholeBytes())
    {
        d.bytes.reserve(d.bits.size() / 8);
        for (std::size_t i = 0; i + 7 < d.bits.size(); i += 8)
        {
            unsigned value = 0;
            for (std::size_t b = 0; b < 8; ++b) value = (value << 1) | d.bits[i + b];
            d.bytes.push_back(static_cast<std::uint8_t>(value));
        }
    }
    return d;
}

BuiltFrame buildFrame(const FrameSpec &spec)
{
    const FrameData data = parseFrameData(spec.data, spec.format);
    if (!data.ok()) return failed("Données : " + data.error);
    const bool replies = spec.protocol == FrameProtocol::Spi ||
                         (spec.protocol == FrameProtocol::I2c && spec.i2cTransfer == I2cTransfer::WriteRead);
    FrameData reply;
    if (replies)
    {
        reply = parseFrameData(spec.reply, spec.format);
        if (!reply.ok()) return failed("Réponse : " + reply.error);
    }
    switch (spec.protocol)
    {
        case FrameProtocol::Bits: return buildBits(spec, data);
        case FrameProtocol::Uart: return buildUart(spec, data);
        case FrameProtocol::I2c: return buildI2c(spec, data, reply);
        case FrameProtocol::Spi: return buildSpi(spec, data, reply);
        case FrameProtocol::Usb: return buildUsb(spec, data);
    }
    return failed("Protocole inconnu.");
}

std::string frameConstruction(const BuiltFrame &frame, std::size_t maximumGroups)
{
    constexpr std::string_view kBar = " \xE2\x94\x82 ";  // │
    constexpr std::string_view kMore = "\xE2\x80\xA6";   // …
    constexpr std::size_t kFieldsShown = 12;              // of a group: the first ones, "…", the last one
    constexpr std::size_t kLongest = 96;                  // characters of a field: its start, "…", its end
    // The groups: a field that starts a group or a line begins one.
    std::vector<std::pair<std::size_t, std::size_t>> groups; // first field, one past the last
    for (std::size_t i = 0; i < frame.fields.size(); ++i)
    {
        if (i == 0 || frame.fields[i].startsGroup || frame.fields[i].startsLine)
            groups.emplace_back(i, i + 1);
        else
            groups.back().second = i + 1;
    }
    const bool twoRows = std::any_of(frame.fields.begin(), frame.fields.end(), [](const FrameField &f) { return !f.reply.empty(); });
    maximumGroups = std::max<std::size_t>(maximumGroups, 3);
    // (The texts of the fields are bits and marks, one byte a character.)
    const auto clipped = [&](const std::string &text) {
        return text.size() <= kLongest ? text : text.substr(0, 64) + std::string(kMore) + text.substr(text.size() - 24);
    };
    std::string top, bottom;
    const auto add = [&](std::size_t g) {
        const auto [first, end] = groups[g];
        if (frame.fields[first].startsLine && !top.empty())
        {
            top += "\n";
        }
        else if (!top.empty())
        {
            top += kBar;
            bottom += kBar;
        }
        for (std::size_t i = first; i < end; ++i)
        {
            if (i > first)
            {
                top += " ";
                bottom += " ";
            }
            if (end - first > kFieldsShown && i == first + kFieldsShown - 2)
            {
                top += kMore;
                bottom += kMore;
                i = end - 2;
                continue;
            }
            top += clipped(frame.fields[i].text);
            bottom += clipped(frame.fields[i].reply);
        }
    };
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        if (groups.size() > maximumGroups && g + 2 == maximumGroups)
        {
            // The first ones, "…", the last one.
            top += std::string(kBar) + std::string(kMore);
            bottom += std::string(kBar) + std::string(kMore);
            g = groups.size() - 2;
            continue;
        }
        add(g);
    }
    if (!twoRows) return top;
    return "MOSI  " + top + "\nMISO  " + bottom;
}

std::string frameTitle(const FrameSpec &spec)
{
    switch (spec.protocol)
    {
        case FrameProtocol::Bits:
            switch (spec.code)
            {
                case LineCode::Nrz: return "Bits NRZ";
                case LineCode::Nrzi: return "Bits NRZI";
                case LineCode::Manchester: return "Bits Manchester";
                case LineCode::Rz: return "Bits RZ";
            }
            return "Bits";
        case FrameProtocol::Uart:
        {
            const char parity = spec.parity == FrameParity::Even ? 'E' : spec.parity == FrameParity::Odd ? 'O' : 'N';
            const char *stop = spec.stopBits >= 1.75 ? "2" : spec.stopBits >= 1.25 ? "1,5" : "1";
            return "UART " + std::to_string(std::clamp(spec.dataBits, 5, 8)) + parity + stop;
        }
        case FrameProtocol::I2c:
        {
            const std::string address = hexByte(spec.i2cAddress & 0x7Fu);
            if (spec.i2cTransfer == I2cTransfer::Read) return "I2C, lecture à " + address;
            if (spec.i2cTransfer == I2cTransfer::WriteRead) return "I2C, écriture puis lecture à " + address;
            return "I2C, écriture à " + address;
        }
        case FrameProtocol::Spi: return "SPI mode " + std::to_string(std::clamp(spec.spiMode, 0, 3));
        case FrameProtocol::Usb:
            return std::string(spec.usbFullSpeed ? "USB plein débit, " : "USB bas débit, ") + std::string(packetInfo(spec.usbPacket).title);
    }
    return "Trame";
}

FrameSpec withFrameProtocol(const FrameSpec &spec, FrameProtocol protocol)
{
    FrameSpec out = spec;
    out.protocol = protocol;
    out.lsbFirst = protocol == FrameProtocol::Uart || protocol == FrameProtocol::Usb;
    return out;
}

double fixedFrameRate(const FrameSpec &spec) noexcept
{
    if (spec.protocol != FrameProtocol::Usb) return 0.0;
    return spec.usbFullSpeed ? 12e6 : 1.5e6;
}

unsigned usbCrc5(unsigned value, int bitCount) noexcept
{
    unsigned crc = 0x1F;
    for (int i = 0; i < bitCount; ++i)
    {
        const bool mix = (((crc ^ (value >> i)) & 1u) != 0);
        crc >>= 1;
        if (mix) crc ^= 0x14u;
    }
    return (crc ^ 0x1Fu) & 0x1Fu;
}

unsigned usbCrc16(std::span<const std::uint8_t> bytes) noexcept
{
    unsigned crc = 0xFFFF;
    for (const std::uint8_t byte : bytes)
    {
        for (int i = 0; i < 8; ++i)
        {
            const bool mix = (((crc ^ (static_cast<unsigned>(byte) >> i)) & 1u) != 0);
            crc >>= 1;
            if (mix) crc ^= 0xA001u;
        }
    }
    return (crc ^ 0xFFFFu) & 0xFFFFu;
}

std::string_view frameProtocolKey(FrameProtocol protocol) noexcept
{
    return keyOf(kProtocolKeys, protocol);
}

bool frameProtocolFromKey(std::string_view key, FrameProtocol &protocol) noexcept
{
    for (const auto &[p, k] : kProtocolKeys)
    {
        if (k != key) continue;
        protocol = p;
        return true;
    }
    return false;
}

std::string_view frameFormatKey(FrameFormat format) noexcept
{
    return keyOf(kFormatKeys, format);
}

std::string_view lineCodeKey(LineCode code) noexcept
{
    return keyOf(kCodeKeys, code);
}

std::string_view frameParityKey(FrameParity parity) noexcept
{
    return keyOf(kParityKeys, parity);
}

std::string_view i2cTransferKey(I2cTransfer transfer) noexcept
{
    return keyOf(kTransferKeys, transfer);
}

std::string_view usbPacketKey(UsbPacket packet) noexcept
{
    return packetInfo(packet).key;
}

void frameToJson(const FrameSpec &s, model::Json &j)
{
    j["protocol"] = frameProtocolKey(s.protocol);
    j["data"] = s.data;
    j["format"] = frameFormatKey(s.format);
    j["lsbFirst"] = s.lsbFirst;
    j["code"] = lineCodeKey(s.code);
    j["dataBits"] = s.dataBits;
    j["parity"] = frameParityKey(s.parity);
    j["stopBits"] = s.stopBits;
    j["i2cAddress"] = s.i2cAddress;
    j["i2cTransfer"] = i2cTransferKey(s.i2cTransfer);
    j["i2cNoAnswer"] = s.i2cNoAnswer;
    j["spiMode"] = s.spiMode;
    j["reply"] = s.reply;
    j["usbFullSpeed"] = s.usbFullSpeed;
    j["usbPacket"] = usbPacketKey(s.usbPacket);
    j["usbAddress"] = s.usbAddress;
    j["usbEndpoint"] = s.usbEndpoint;
    j["usbFrameNumber"] = s.usbFrameNumber;
}

FrameSpec frameFromJson(const model::Json &j)
{
    FrameSpec s;
    if (!j.isObject()) return s;
    constexpr std::size_t kLongest = 65536; // characters of the data typed
    const auto whole = [&j](std::string_view key, double fallback, double low, double high) {
        const model::Json &v = j[key];
        const double x = v.isNumber() ? v.toNumber(fallback) : fallback;
        return std::isfinite(x) ? std::floor(std::clamp(x, low, high)) : fallback;
    };
    const auto text = [&j](std::string_view key, std::string fallback) {
        std::string value = j[key].isString() ? j[key].toString() : std::move(fallback);
        if (value.size() > kLongest) value.resize(kLongest);
        return value;
    };
    s.protocol = fromKey(kProtocolKeys, j["protocol"].toString(), s.protocol);
    s.data = text("data", s.data);
    s.format = fromKey(kFormatKeys, j["format"].toString(), s.format);
    s.lsbFirst = j["lsbFirst"].toBool(s.lsbFirst);
    s.code = fromKey(kCodeKeys, j["code"].toString(), s.code);
    s.dataBits = static_cast<int>(whole("dataBits", 8.0, 5.0, 8.0));
    s.parity = fromKey(kParityKeys, j["parity"].toString(), s.parity);
    const double stop = j["stopBits"].isNumber() ? j["stopBits"].toNumber(1.0) : 1.0;
    s.stopBits = stop >= 1.75 ? 2.0 : stop >= 1.25 ? 1.5 : 1.0;
    s.i2cAddress = static_cast<unsigned>(whole("i2cAddress", 0x48, 0.0, 127.0));
    s.i2cTransfer = fromKey(kTransferKeys, j["i2cTransfer"].toString(), s.i2cTransfer);
    s.i2cNoAnswer = j["i2cNoAnswer"].toBool(false);
    s.spiMode = static_cast<int>(whole("spiMode", 0.0, 0.0, 3.0));
    s.reply = text("reply", std::string());
    s.usbFullSpeed = j["usbFullSpeed"].toBool(false);
    const std::string packet = j["usbPacket"].toString();
    for (const PacketInfo &info : kPackets)
        if (info.key == packet) s.usbPacket = info.packet;
    s.usbAddress = static_cast<unsigned>(whole("usbAddress", 0.0, 0.0, 127.0));
    s.usbEndpoint = static_cast<unsigned>(whole("usbEndpoint", 0.0, 0.0, 15.0));
    s.usbFrameNumber = static_cast<unsigned>(whole("usbFrameNumber", 0.0, 0.0, 2047.0));
    return s;
}
} // namespace qacq::analysis
