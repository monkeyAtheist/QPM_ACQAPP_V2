// Digital frames of the generator: the data typed, the frame each protocol builds from them (read back here by a
// receiver of its own: where the clock says, between start and stop, after NRZI and bit stuffing), how it is
// shown, and its keys.
#include "qacq_test.h"

#include "qacq/analysis/frames.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace qacq::analysis;
using qacq::model::Json;

namespace
{
using Levels = std::vector<std::uint8_t>;

std::string text(const Levels &levels)
{
    std::string s;
    for (const std::uint8_t v : levels) s += v ? '1' : '0';
    return s;
}

bool contains(const std::string &whole, const std::string &part)
{
    return whole.find(part) != std::string::npos;
}

// A line as a receiver sees it: at rest before and after the frame.
Levels withRest(const BuiltFrame &frame, std::size_t line, std::size_t rest = 3)
{
    Levels out(rest, frame.idle[line]);
    out.insert(out.end(), frame.levels[line].begin(), frame.levels[line].end());
    out.insert(out.end(), rest, frame.idle[line]);
    return out;
}

FrameSpec spec(FrameProtocol protocol, std::string data)
{
    FrameSpec s = withFrameProtocol(FrameSpec{}, protocol);
    s.data = std::move(data);
    return s;
}

// What an I2C bus carried: "S", "P", and the bits taken when SCL rises (kept when it falls: SDA moving while SCL
// is high is a start or a stop, not a bit). Both lines moving at once is a fault ("!").
std::string i2cTrace(const BuiltFrame &frame)
{
    const Levels sda = withRest(frame, 0), scl = withRest(frame, 1);
    std::string trace;
    char taken = 0;
    for (std::size_t i = 1; i < sda.size(); ++i)
    {
        if (scl[i - 1] != scl[i] && sda[i - 1] != sda[i]) trace += '!';
        if (!scl[i - 1] && scl[i]) taken = sda[i] ? '1' : '0';
        if (scl[i - 1] && !scl[i] && taken)
        {
            trace += taken;
            taken = 0;
        }
        if (scl[i - 1] && scl[i] && sda[i - 1] != sda[i])
        {
            trace += sda[i] ? 'P' : 'S';
            taken = 0;
        }
    }
    return trace;
}

// What an SPI slave reads on MOSI and a master on MISO: the bits taken on the edge of the mode while CS is low.
struct SpiTrace
{
    std::string mosi, miso;
    std::size_t edges = 0;   // of the clock, while selected
    bool restOk = true;      // the clock at its level of rest when CS moves
};
SpiTrace spiTrace(const BuiltFrame &frame, int mode)
{
    const Levels mosi = withRest(frame, 0), miso = withRest(frame, 1), clock = withRest(frame, 2), cs = withRest(frame, 3);
    const std::uint8_t rest = mode >= 2 ? 1 : 0;
    const bool rising = mode == 0 || mode == 3;
    SpiTrace t;
    for (std::size_t i = 1; i < cs.size(); ++i)
    {
        if (cs[i - 1] != cs[i] && (clock[i - 1] != rest || clock[i] != rest)) t.restOk = false;
        if (cs[i] || cs[i - 1]) continue;
        if (clock[i - 1] == clock[i]) continue;
        ++t.edges;
        if ((clock[i] != 0) != rising) continue;
        // Taken on this edge: the data must have been there before it, and still be.
        if (mosi[i - 1] != mosi[i] || miso[i - 1] != miso[i]) t.restOk = false;
        t.mosi += mosi[i] ? '1' : '0';
        t.miso += miso[i] ? '1' : '0';
    }
    return t;
}

// The packets on a USB pair: each as the bits between its SYNC and its end, NRZI undone and stuffed bits taken
// out ("x" where a stuffed bit is missing or wrong).
std::vector<std::string> usbPackets(const BuiltFrame &frame, bool fullSpeed)
{
    const Levels dp = withRest(frame, 0), dm = withRest(frame, 1);
    std::vector<std::string> packets;
    std::string bits;
    bool inside = false;
    int ones = 0;
    // J: the state of rest.
    const auto isJ = [&](std::size_t i) { return fullSpeed ? (dp[i] && !dm[i]) : (!dp[i] && dm[i]); };
    for (std::size_t i = 1; i < dp.size(); ++i)
    {
        const bool se0 = !dp[i] && !dm[i];
        if (se0)
        {
            if (inside) packets.push_back(bits);
            inside = false;
            bits.clear();
            continue;
        }
        const bool before0 = !dp[i - 1] && !dm[i - 1];
        if (before0) continue; // the J after the end of a packet
        const bool same = isJ(i) == isJ(i - 1);
        if (!inside)
        {
            if (same) continue; // at rest
            inside = true;
            ones = 0;
        }
        if (ones == 6)
        {
            // A stuffed bit: a change of state, not data.
            if (same) bits += 'x';
            ones = 0;
            continue;
        }
        bits += same ? '1' : '0';
        ones = same ? ones + 1 : 0;
    }
    return packets;
}

// The bytes of a packet after its SYNC (bits from the least significant), "?" for bits left over.
std::string usbBytes(const std::string &packet)
{
    if (packet.size() < 8 || packet.substr(0, 8) != "00000001") return "no sync";
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    std::size_t i = 8;
    for (; i + 8 <= packet.size(); i += 8)
    {
        unsigned value = 0;
        for (std::size_t b = 0; b < 8; ++b)
            if (packet[i + b] == '1') value |= 1u << b;
        if (!out.empty()) out += ' ';
        out += kDigits[value >> 4];
        out += kDigits[value & 0xF];
    }
    if (i != packet.size()) out += " ?";
    return out;
}
} // namespace

TEST_CASE("frames: data typed as bits, hexadecimal, decimal or text", "[frames]")
{
    // Bits: the groups typed.
    FrameData d = parseFrameData("0011 0010 100", FrameFormat::Auto);
    REQUIRE(d.ok());
    CHECK(d.format == FrameFormat::Binary);
    CHECK(text(d.bits) == "00110010100");
    CHECK((d.groups == std::vector<std::size_t>{4, 4, 3}));
    CHECK(!d.wholeBytes());
    CHECK(d.bytes.empty());
    d = parseFrameData("00110010100", FrameFormat::Auto);
    CHECK((d.groups == std::vector<std::size_t>{11}));
    d = parseFrameData("0b1010_0011, 1111'0000", FrameFormat::Auto);
    CHECK(d.format == FrameFormat::Binary);
    CHECK((d.groups == std::vector<std::size_t>{8, 8}));
    CHECK((d.bytes == Levels{0xA3, 0xF0}));
    // Hexadecimal: bytes, in the order written.
    d = parseFrameData("0x2831", FrameFormat::Auto);
    REQUIRE(d.ok());
    CHECK(d.format == FrameFormat::Hex);
    CHECK((d.bytes == Levels{0x28, 0x31}));
    CHECK(text(d.bits) == "0010100000110001");
    CHECK((d.groups == std::vector<std::size_t>{8, 8}));
    CHECK((parseFrameData("28 31", FrameFormat::Hex).bytes == Levels{0x28, 0x31}));
    CHECK((parseFrameData("0x28, 0x31; 28:31-28h", FrameFormat::Hex).bytes == Levels{0x28, 0x31, 0x28, 0x31, 0x28}));
    CHECK((parseFrameData("f 123", FrameFormat::Hex).bytes == Levels{0x0F, 0x01, 0x23})); // an odd number of digits: a 0 in front
    CHECK((parseFrameData("hex: de ad BE EF", FrameFormat::Auto).bytes == Levels{0xDE, 0xAD, 0xBE, 0xEF}));
    // Decimal: a byte a number.
    CHECK((parseFrameData("40 49, 255", FrameFormat::Decimal).bytes == Levels{40, 49, 255}));
    CHECK((parseFrameData("dec : 40 49", FrameFormat::Auto).bytes == Levels{40, 49}));
    // Text: its bytes (UTF-8), the quotation marks around it and the name of its format taken off.
    d = parseFrameData("Bonjour", FrameFormat::Auto);
    CHECK(d.format == FrameFormat::Text);
    CHECK((d.bytes == Levels{'B', 'o', 'n', 'j', 'o', 'u', 'r'}));
    CHECK((parseFrameData("ascii : \"bonjour\"", FrameFormat::Auto).bytes == Levels{'b', 'o', 'n', 'j', 'o', 'u', 'r'}));
    CHECK((parseFrameData("\"0101\"", FrameFormat::Auto).bytes == Levels{'0', '1', '0', '1'})); // quoted: text
    CHECK((parseFrameData("« ok »", FrameFormat::Auto).bytes == Levels{'o', 'k'}));
    CHECK((parseFrameData("'a b'", FrameFormat::Auto).bytes == Levels{'a', ' ', 'b'}));
    CHECK((parseFrameData("\"x\"", FrameFormat::Text).bytes == Levels{'"', 'x', '"'})); // said text: kept as typed
    CHECK(parseFrameData("12 300", FrameFormat::Auto).format == FrameFormat::Text);  // not only 0 and 1
    CHECK((parseFrameData("é", FrameFormat::Auto).bytes == Levels{0xC3, 0xA9}));
    CHECK((parseFrameData("a\\n\\r\\t\\0\\\\\\x41\\q", FrameFormat::Text).bytes == Levels{'a', '\n', '\r', '\t', 0, '\\', 'A', '\\', 'q'}));
    CHECK((parseFrameData(" a ", FrameFormat::Text).bytes == Levels{' ', 'a', ' '}));
    // Nothing: no bit, no error.
    d = parseFrameData("  ", FrameFormat::Auto);
    CHECK(d.ok());
    CHECK(d.bits.empty());
    CHECK(d.wholeBytes());
    // What is wrong, said.
    CHECK(parseFrameData("0102", FrameFormat::Binary).error ==
          "« 2 » n'est pas un chiffre binaire (0 ou 1) ; pour du texte ou de l'hexadécimal, changez le format.");
    CHECK(parseFrameData("0x2G", FrameFormat::Auto).error == "« G » n'est pas un chiffre hexadécimal (0 à 9, A à F).");
    CHECK(parseFrameData("12 300", FrameFormat::Decimal).error == "« 300 » n'est pas un nombre de 0 à 255 (un octet par nombre).");
    CHECK(parseFrameData("1é", FrameFormat::Binary).error.starts_with("« é »"));
    CHECK(parseFrameData("0102", FrameFormat::Binary).bits.empty());
    // No more than 4 096 bytes.
    CHECK(parseFrameData(std::string(kMaximumFrameBytes, 'a'), FrameFormat::Text).ok());
    CHECK(parseFrameData(std::string(kMaximumFrameBytes + 1, 'a'), FrameFormat::Text).error == "au plus 4 096 octets (4 097 tapés).");
}

TEST_CASE("frames: bits as they are, on a line code, with their clock", "[frames]")
{
    // "00110010100": a square made of the bits, most significant (the left one) first.
    FrameSpec s;
    s.data = "00110010100";
    BuiltFrame f = buildFrame(s);
    REQUIRE(f.ok());
    CHECK(f.ticksPerBit == 2);
    CHECK(f.tickCount == 22);
    CHECK(f.bitCount() == 11.0);
    REQUIRE(f.levels.size() == 2);
    CHECK(text(f.levels[0]) == "0000111100001100110000");
    CHECK(text(f.levels[1]) == "0101010101010101010101"); // the clock rises in the middle of each bit
    CHECK((f.idle == Levels{0, 0}));
    CHECK(f.summary == "11 bits");
    CHECK(f.format == FrameFormat::Binary);
    REQUIRE(f.fields.size() == 1);
    CHECK(f.fields[0].text == "00110010100");
    CHECK(f.fields[0].firstTick == 0);
    CHECK(f.fields[0].tickCount == 22);
    CHECK(frameConstruction(f) == "00110010100");
    CHECK(frameTitle(s) == "Bits NRZ");
    // Least significant first: each group from its right bit.
    s.lsbFirst = true;
    f = buildFrame(s);
    CHECK(text(f.levels[0]) == "0000110011000011110000");
    CHECK(f.fields[0].text == "00101001100");
    s.data = "0x2831";
    f = buildFrame(s);
    CHECK(frameConstruction(f) == "00010100 │ 10001100"); // each byte turned round, the bytes in their order
    CHECK(f.summary == "2 octets : 28 31");
    CHECK(f.byteCount == 2);
    CHECK(f.fields[1].note == "0x31 « 1 »");
    s.lsbFirst = false;
    CHECK(frameConstruction(buildFrame(s)) == "00101000 │ 00110001");
    // The line codes, on 1011.
    s.data = "1011";
    s.code = LineCode::Nrzi;
    CHECK(text(buildFrame(s).levels[0]) == "11110011"); // a 1 changes the level, from low
    CHECK(frameTitle(s) == "Bits NRZI");
    s.code = LineCode::Manchester;
    CHECK(text(buildFrame(s).levels[0]) == "01100101"); // 1: low then high; 0: high then low
    s.code = LineCode::Rz;
    CHECK(text(buildFrame(s).levels[0]) == "10001010");
    CHECK(contains(buildFrame(s).legend, "RZ : un 1 est haut pendant la première moitié du bit"));
    // Text is bytes; nothing typed is said.
    s = FrameSpec{};
    CHECK(s.data == "Bonjour");
    f = buildFrame(s);
    CHECK(f.tickCount == 2 * 56);
    CHECK(f.summary == "7 octets : 42 6F 6E 6A 6F 75 72");
    CHECK(f.fields[0].note == "0x42 « B »");
    s.data.clear();
    CHECK(buildFrame(s).error ==
          "Aucune donnée à envoyer : tapez des bits (0011 0010), de l'hexadécimal (0x2831) ou du texte (Bonjour).");
    s.data = "0x2G";
    CHECK(buildFrame(s).error == "Données : « G » n'est pas un chiffre hexadécimal (0 à 9, A à F).");
}

TEST_CASE("frames: a UART frame: start, data from the least significant bit, parity, stop", "[frames]")
{
    FrameSpec s = spec(FrameProtocol::Uart, "B");
    CHECK(s.lsbFirst);
    BuiltFrame f = buildFrame(s);
    REQUIRE(f.ok());
    REQUIRE(f.levels.size() == 1);
    CHECK((f.idle == Levels{1}));
    // 0x42 = 0100 0010: start 0, then 0 1 0 0 0 0 1 0, stop 1; two ticks a bit.
    CHECK(text(f.levels[0]) == "00" "0011000000001100" "11");
    CHECK(f.bitCount() == 10.0);
    CHECK(frameConstruction(f) == "St 01000010 Sp");
    CHECK(frameTitle(s) == "UART 8N1");
    CHECK(f.legend == "St : départ (0) · Sp : arrêt (1) · données dans l'ordre d'envoi, poids faible d'abord.");
    REQUIRE(f.fields.size() == 3);
    CHECK(f.fields[0].note == "Bit de départ (0)");
    CHECK(f.fields[1].firstTick == 2);
    CHECK(f.fields[1].tickCount == 16);
    CHECK(f.fields[2].note == "Bit d'arrêt (1)");
    // Even parity: an even number of ones with the data (0x42 has two: 0); odd: the other; 1.5 and 2 stop bits.
    s.parity = FrameParity::Even;
    s.stopBits = 1.5;
    f = buildFrame(s);
    CHECK(frameConstruction(f) == "St 01000010 P0 Sp");
    CHECK(f.bitCount() == 11.5);
    CHECK(text(f.levels[0]).ends_with("00" "111"));
    CHECK(frameTitle(s) == "UART 8E1,5");
    s.parity = FrameParity::Odd;
    s.stopBits = 2.0;
    f = buildFrame(s);
    CHECK(frameConstruction(f) == "St 01000010 P1 Sp");
    CHECK(f.bitCount() == 12.0);
    CHECK(frameTitle(s) == "UART 8O2");
    s.data = "0x07";
    CHECK(frameConstruction(buildFrame(s)) == "St 11100000 P0 Sp"); // three ones, and none to make them odd
    // Several bytes: a frame each; most significant bit first when asked.
    s = spec(FrameProtocol::Uart, "0x80 0x41");
    f = buildFrame(s);
    CHECK(frameConstruction(f) == "St 00000001 Sp │ St 10000010 Sp");
    CHECK(f.bitCount() == 20.0);
    s.lsbFirst = false;
    CHECK(frameConstruction(buildFrame(s)) == "St 10000000 Sp │ St 01000001 Sp");
    // Seven data bits: what does not fit is said.
    s.lsbFirst = true;
    s.dataBits = 7;
    f = buildFrame(s);
    CHECK(frameConstruction(f) == "St 0000000 Sp │ St 1000001 Sp");
    REQUIRE(f.warnings.size() == 1);
    CHECK(f.warnings[0] == "0x80 ne tient pas sur 7 bits : les bits de poids fort ne sont pas envoyés.");
    s.dataBits = 5;
    s.data = "0x1F";
    f = buildFrame(s);
    CHECK(frameConstruction(f) == "St 11111 Sp");
    CHECK(f.warnings.empty());
    // Bytes only.
    s.data = "00110010100";
    CHECK(buildFrame(s).error == "L'UART envoie des octets : tapez les bits par huit (11 bits tapés).");
    s.data.clear();
    CHECK(buildFrame(s).error.starts_with("Aucune donnée à envoyer"));
}

TEST_CASE("frames: an I2C exchange, read back from SDA and SCL", "[frames]")
{
    // A write of two bytes at 0x48: start, 1001000 0, acknowledged, each byte and its acknowledge, stop.
    FrameSpec s = spec(FrameProtocol::I2c, "0x01 0xA5");
    CHECK(!s.lsbFirst);
    BuiltFrame f = buildFrame(s);
    REQUIRE(f.ok());
    CHECK(f.ticksPerBit == 4);
    CHECK((f.idle == Levels{1, 1}));
    CHECK(f.bitCount() == 29.0); // start, 9 bits of address, 2 × 9, stop
    CHECK(i2cTrace(f) == "S" "10010000" "0" "00000001" "0" "10100101" "0" "P");
    CHECK(frameConstruction(f) == "S 1001000 W A │ 00000001 A │ 10100101 A │ P");
    CHECK(frameTitle(s) == "I2C, écriture à 0x48");
    CHECK(f.summary == "2 octets écrits : 01 A5");
    CHECK(contains(f.legend, "W : écriture (0)"));
    // A read: the direction bit at 1, the master acknowledges each byte but the last.
    s.i2cTransfer = I2cTransfer::Read;
    f = buildFrame(s);
    CHECK(i2cTrace(f) == "S" "10010001" "0" "00000001" "0" "10100101" "1" "P");
    CHECK(frameConstruction(f) == "S 1001000 R A │ 00000001 A │ 10100101 N │ P");
    CHECK(f.summary == "2 octets lus : 01 A5");
    CHECK(frameTitle(s) == "I2C, lecture à 0x48");
    // A write then a read: a repeated start between them, no stop.
    s.i2cTransfer = I2cTransfer::WriteRead;
    s.data = "0x01";
    s.reply = "0x7F 0x80";
    s.i2cAddress = 0x50;
    f = buildFrame(s);
    REQUIRE(f.ok());
    CHECK(i2cTrace(f) == "S" "10100000" "0" "00000001" "0" "S" "10100001" "0" "01111111" "0" "10000000" "1" "P");
    CHECK(frameConstruction(f) == "S 1010000 W A │ 00000001 A │ Sr 1010000 R A │ 01111111 A │ 10000000 N │ P");
    CHECK(f.summary == "1 octet écrit : 01 ; 2 octets lus : 7F 80");
    CHECK(f.bitCount() == 48.0);
    s.reply.clear();
    CHECK(buildFrame(s).error == "Réponse : tapez les octets lus après le redémarrage.");
    s.reply = "0x7G";
    CHECK(buildFrame(s).error == "Réponse : « G » n'est pas un chiffre hexadécimal (0 à 9, A à F).");
    // Nobody answers: the address, not acknowledged, and the stop.
    s.reply = "0x7F 0x80";
    s.i2cNoAnswer = true;
    f = buildFrame(s);
    REQUIRE(f.ok());
    CHECK(i2cTrace(f) == "S" "10100000" "1" "P");
    CHECK(frameConstruction(f) == "S 1010000 W N │ P");
    // The address alone (a probe); a read needs bytes; bits by eight.
    s = spec(FrameProtocol::I2c, "");
    f = buildFrame(s);
    REQUIRE(f.ok());
    CHECK(i2cTrace(f) == "S" "10010000" "0" "P");
    CHECK(f.summary == "adresse seule");
    s.i2cTransfer = I2cTransfer::Read;
    CHECK(buildFrame(s).error == "Tapez les octets que l'esclave renvoie : une lecture en lit au moins un.");
    s.data = "101";
    CHECK(buildFrame(s).error == "L'I2C envoie des octets : tapez les bits par huit (3 bits tapés).");
    // The order of the bits is that of the bus, whatever is asked.
    s = spec(FrameProtocol::I2c, "0x01");
    s.lsbFirst = true;
    CHECK(i2cTrace(buildFrame(s)) == "S" "10010000" "0" "00000001" "0" "P");
}

TEST_CASE("frames: an SPI exchange in its four modes, read on the edges of the clock", "[frames]")
{
    for (int mode = 0; mode < 4; ++mode)
    {
        FrameSpec s = spec(FrameProtocol::Spi, "0x9F 0x00");
        s.reply = "0x00 0xEF";
        s.spiMode = mode;
        const BuiltFrame f = buildFrame(s);
        REQUIRE(f.ok());
        REQUIRE(f.levels.size() == 4);
        CHECK((f.idle == Levels{0, 0, static_cast<std::uint8_t>(mode >= 2 ? 1 : 0), 1}));
        CHECK(f.bitCount() == 17.0); // half a bit, sixteen, half a bit
        const SpiTrace t = spiTrace(f, mode);
        CHECK(t.mosi == "1001111100000000");
        CHECK(t.miso == "0000000011101111");
        CHECK(t.edges == 32);
        CHECK(t.restOk);
        CHECK((std::all_of(f.levels[3].begin(), f.levels[3].end(), [](std::uint8_t v) { return v == 0; }))); // selected all along
        CHECK(frameTitle(s) == "SPI mode " + std::to_string(mode));
        CHECK(frameConstruction(f) == "MOSI  10011111 │ 00000000\nMISO  00000000 │ 11101111");
        CHECK(f.summary == "2 octets échangés : MOSI 9F 00 ; MISO 00 EF");
    }
    // No reply: MISO stays low, one row. Least significant bit first when asked.
    FrameSpec s = spec(FrameProtocol::Spi, "0x9F");
    BuiltFrame f = buildFrame(s);
    CHECK(frameConstruction(f) == "10011111");
    CHECK(spiTrace(f, 0).miso == "00000000");
    CHECK(f.summary == "1 octet : 9F");
    CHECK(contains(f.legend, "Mode 0 : horloge au repos à 0, données lues sur le front montant (le premier)"));
    s.lsbFirst = true;
    CHECK(spiTrace(buildFrame(s), 0).mosi == "11111001");
    // The shorter of the two is completed with zeros.
    s = spec(FrameProtocol::Spi, "0x9F");
    s.reply = "0x00 0xEF 0x40";
    f = buildFrame(s);
    CHECK(spiTrace(f, 0).mosi == "100111110000000000000000");
    CHECK(spiTrace(f, 0).miso == "000000001110111101000000");
    CHECK(f.fields[1].note == "MOSI 0x00 (rien à envoyer) · MISO 0xEF");
    s.data = "101";
    CHECK(buildFrame(s).error == "Le SPI envoie des octets : tapez les bits par huit (3 bits tapés).");
    s.data.clear();
    s.reply.clear();
    CHECK(buildFrame(s).error.starts_with("Aucune donnée à envoyer"));
}

TEST_CASE("frames: the CRC of USB, on its published examples", "[frames]")
{
    // As sent: from bit 0 of the value. (Examples of "Cyclic Redundancy Checks in USB".)
    const auto sent = [](unsigned crc, int count) {
        std::string s;
        for (int i = 0; i < count; ++i) s += ((crc >> i) & 1u) ? '1' : '0';
        return s;
    };
    CHECK(sent(usbCrc5(0x15u | (0xEu << 7), 11), 5) == "10111"); // address 0x15, endpoint 0xE
    CHECK(sent(usbCrc5(0x3Au | (0xAu << 7), 11), 5) == "11100");
    CHECK(sent(usbCrc5(0x70u | (0x4u << 7), 11), 5) == "01110");
    CHECK(sent(usbCrc5(0x710, 11), 5) == "10100");               // frame number 0x710
    CHECK(usbCrc5(0, 11) == 0x02);                               // the token 2D 00 10
    const std::uint8_t first[] = {0x00, 0x01, 0x02, 0x03}, second[] = {0x23, 0x45, 0x67, 0x89};
    CHECK(sent(usbCrc16(first), 16) == "1111011101011110");
    CHECK(sent(usbCrc16(second), 16) == "0111000000111000");
    const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(usbCrc16(digits) == 0xB4C8); // CRC-16/USB
    CHECK((usbCrc16({}) == 0x0000));
}

TEST_CASE("frames: USB packets, read back from D+ and D−", "[frames]")
{
    // The SETUP token to address 0, endpoint 0, at low speed: 2D 00 10.
    FrameSpec s = spec(FrameProtocol::Usb, "");
    CHECK(s.usbPacket == UsbPacket::Setup);
    BuiltFrame f = buildFrame(s);
    REQUIRE(f.ok());
    CHECK(f.ticksPerBit == 1);
    CHECK((f.idle == Levels{0, 1})); // low speed: J is D− high
    CHECK(f.tickCount == 35);      // 8 + 8 + 7 + 4 + 5, two bits of SE0 and a J
    std::vector<std::string> packets = usbPackets(f, false);
    REQUIRE(packets.size() == 1);
    CHECK(usbBytes(packets[0]) == "2D 00 10");
    CHECK(text(f.levels[0]).starts_with("10101011")); // SYNC: KJKJKJKK, K being D+ high at low speed
    CHECK(text(f.levels[0]).ends_with("000"));        // SE0, SE0, J
    CHECK(text(f.levels[1]).ends_with("001"));
    CHECK(frameConstruction(f) == "SYNC 00000001 │ PID 10110100 │ ADDR 0000000 │ ENDP 0000 │ CRC5 01000 │ EOP");
    CHECK(frameTitle(s) == "USB bas débit, jeton SETUP");
    CHECK(f.summary == "adresse 0, point de terminaison 0");
    CHECK(fixedFrameRate(s) == 1.5e6);
    // Full speed: the other polarity; IN to address 0x15, endpoint 0xE.
    s.usbFullSpeed = true;
    s.usbPacket = UsbPacket::In;
    s.usbAddress = 0x15;
    s.usbEndpoint = 0xE;
    f = buildFrame(s);
    CHECK((f.idle == Levels{1, 0}));
    CHECK(text(f.levels[0]).starts_with("01010100"));
    packets = usbPackets(f, true);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == "00000001" "10010110" "1010100" "0111" "10111");
    CHECK(fixedFrameRate(s) == 12e6);
    CHECK(frameTitle(s) == "USB plein débit, jeton IN");
    // The other tokens and the handshakes: their PID and its complement.
    const std::pair<UsbPacket, const char *> pids[] = {{UsbPacket::Out, "E1 15 EF"}, {UsbPacket::Ack, "D2"},
                                                       {UsbPacket::Nak, "5A"},      {UsbPacket::Stall, "1E"}};
    for (const auto &[packet, bytes] : pids)
    {
        s.usbPacket = packet;
        packets = usbPackets(buildFrame(s), true);
        REQUIRE(packets.size() == 1);
        CHECK(usbBytes(packets[0]) == bytes);
    }
    CHECK(buildFrame(s).tickCount == 19); // a handshake: SYNC, PID, end
    // Start of frame 0x710.
    s.usbPacket = UsbPacket::Sof;
    s.usbFrameNumber = 0x710;
    f = buildFrame(s);
    packets = usbPackets(f, true);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == "00000001" "10100101" "00001000111" "10100");
    CHECK(f.warnings.empty());
    s.usbFullSpeed = false;
    CHECK(buildFrame(s).warnings.size() == 1); // no SOF at low speed
    // Data: the bytes and their CRC16; six ones in a row are followed by a stuffed zero.
    s = spec(FrameProtocol::Usb, "0x00 0x01 0x02 0x03");
    s.usbPacket = UsbPacket::Data1;
    f = buildFrame(s);
    packets = usbPackets(f, false);
    REQUIRE(packets.size() == 1);
    CHECK(usbBytes(packets[0]) == "4B 00 01 02 03 EF 7A");
    CHECK(f.tickCount == 8 + 8 + 32 + 16 + 3);
    CHECK(!contains(f.legend, "bourrage"));
    s.data = "0xFF 0xFF 0x00";
    s.usbPacket = UsbPacket::Data0;
    f = buildFrame(s);
    packets = usbPackets(f, false);
    REQUIRE(packets.size() == 1);
    CHECK(!contains(packets[0], "x"));
    CHECK(usbBytes(packets[0]).starts_with("C3 FF FF 00 "));
    CHECK(contains(frameConstruction(f), "DATA 1111[0]1111 11[0]111111[0] 00000000"));
    CHECK(contains(f.legend, "[0] : bit de bourrage après six 1"));
    CHECK(f.tickCount > 8 + 8 + 24 + 16 + 3); // the stuffed bits
    // Never more than six ticks without a change, whatever the data.
    s.data = std::string(64, '\xFF');
    s.format = FrameFormat::Text;
    f = buildFrame(s);
    std::size_t longest = 0, run = 0;
    for (std::size_t i = 1; i + 3 < f.tickCount; ++i)
    {
        run = f.levels[0][i] == f.levels[0][i - 1] ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    CHECK(longest == 6);
    packets = usbPackets(f, false);
    REQUIRE(packets.size() == 1);
    CHECK(!contains(packets[0], "x"));
    CHECK(packets[0].size() == 8 + 8 + 64 * 8 + 16);
    CHECK(f.warnings.size() == 1); // more than 8 bytes at low speed
    CHECK(f.warnings[0] == "En bas débit, un paquet de données porte au plus 8 octets (64 ici).");
    // An empty data packet: its CRC16 alone.
    s = spec(FrameProtocol::Usb, "");
    s.usbPacket = UsbPacket::Data0;
    packets = usbPackets(buildFrame(s), false);
    REQUIRE(packets.size() == 1);
    CHECK(usbBytes(packets[0]) == "C3 00 00");
    // A SETUP transaction: the token, the request in DATA0, the acknowledge.
    s.usbPacket = UsbPacket::SetupTransaction;
    s.data = "0x80 0x06 0x00 0x01 0x00 0x00 0x12 0x00";
    f = buildFrame(s);
    REQUIRE(f.ok());
    packets = usbPackets(f, false);
    REQUIRE(packets.size() == 3);
    CHECK(usbBytes(packets[0]) == "2D 00 10");
    CHECK(usbBytes(packets[1]) == "C3 80 06 00 01 00 00 12 00 E0 F4");
    CHECK(usbBytes(packets[2]) == "D2");
    CHECK(f.warnings.empty());
    const std::string shown = frameConstruction(f);
    CHECK(std::count(shown.begin(), shown.end(), '\n') == 2); // a packet a line
    CHECK(contains(shown, "\nSYNC 00000001 │ PID 11000011 │ DATA 00000001 01100000"));
    CHECK(frameTitle(s) == "USB bas débit, transaction SETUP");
    s.data = "0x80 0x06";
    CHECK((buildFrame(s).warnings == std::vector<std::string>{"Une requête SETUP compte 8 octets (2 ici)."}));
    s.data.clear();
    CHECK(buildFrame(s).error.starts_with("Tapez les 8 octets de la requête"));
    s.data = "101";
    CHECK(buildFrame(s).error == "L'USB envoie des octets : tapez les bits par huit (3 bits tapés).");
    // A token carries no data: what is typed does not matter.
    s.usbPacket = UsbPacket::Setup;
    CHECK(buildFrame(s).ok());
}

TEST_CASE("frames: a long frame is shown by its first fields and its last", "[frames]")
{
    FrameSpec s = spec(FrameProtocol::Uart, std::string(40, 'U')); // 0x55: 10101010 from its least significant bit
    const BuiltFrame f = buildFrame(s);
    REQUIRE(f.ok());
    const std::string all = frameConstruction(f, 1000);
    CHECK(std::count(all.begin(), all.end(), 'S') == 2 * 40); // St and Sp, forty times
    const std::string shown = frameConstruction(f, 6);
    CHECK(shown == "St 10101010 Sp │ St 10101010 Sp │ St 10101010 Sp │ St 10101010 Sp │ … │ St 10101010 Sp");
    CHECK(frameConstruction(f, 40) == all);
    CHECK(frameConstruction(f) != all); // 24 groups by default
    CHECK((frameConstruction(BuiltFrame{}).empty()));
    // The many bytes of a USB data packet: the first ten, "…", the last one; a long run of bits: its two ends.
    s = spec(FrameProtocol::Usb, std::string(40, '\x01'));
    s.format = FrameFormat::Text;
    s.usbPacket = UsbPacket::Data0;
    s.usbFullSpeed = true;
    std::string ten = "DATA 10000000";
    for (int i = 0; i < 9; ++i) ten += " 10000000";
    CHECK(contains(frameConstruction(buildFrame(s)), "PID 11000011 │ " + ten + " … 10000000 │ CRC16 "));
    FrameSpec bits;
    bits.data = std::string(300, '1');
    CHECK(frameConstruction(buildFrame(bits)) == std::string(64, '1') + "…" + std::string(24, '1'));
}

TEST_CASE("frames: the lines of a protocol, their keys, and the recipes", "[frames]")
{
    CHECK(frameLines(FrameProtocol::Bits).size() == 2);
    CHECK(frameLines(FrameProtocol::Uart).size() == 1);
    CHECK(frameLines(FrameProtocol::I2c).size() == 2);
    CHECK(frameLines(FrameProtocol::Spi).size() == 4);
    CHECK(frameLines(FrameProtocol::Usb).size() == 2);
    CHECK(frameLines(FrameProtocol::I2c)[1].name == "SCL");
    CHECK(frameLines(FrameProtocol::Usb)[1].name == "D−");
    CHECK(frameLineIndex(FrameProtocol::Spi, "sclk") == 2);
    CHECK(frameLineIndex(FrameProtocol::Spi, "sda") == 0); // not one of its lines: the first
    CHECK(frameLineIndex(FrameProtocol::Bits, "") == 0);
    // From one protocol to another, a line keeps its part: the clock stays a clock.
    CHECK(frameLineFor(FrameProtocol::I2c, "scl", FrameProtocol::Spi) == "sclk");
    CHECK(frameLineFor(FrameProtocol::I2c, "sda", FrameProtocol::Spi) == "mosi");
    CHECK(frameLineFor(FrameProtocol::Spi, "miso", FrameProtocol::Usb) == "dm");
    CHECK(frameLineFor(FrameProtocol::Spi, "cs", FrameProtocol::I2c) == "sda"); // no such part: the first line
    CHECK(frameLineFor(FrameProtocol::Bits, "clock", FrameProtocol::Uart) == "tx");
    CHECK(frameLineFor(FrameProtocol::Usb, "dp", FrameProtocol::Bits) == "data");
    // Another protocol: the same data, its usual order of bits.
    FrameSpec s;
    s.data = "0x41";
    CHECK(withFrameProtocol(s, FrameProtocol::Uart).lsbFirst);
    CHECK(withFrameProtocol(s, FrameProtocol::Uart).data == "0x41");
    CHECK(!withFrameProtocol(withFrameProtocol(s, FrameProtocol::Uart), FrameProtocol::Spi).lsbFirst);
    CHECK(fixedFrameRate(s) == 0.0);
    CHECK(fixedFrameRate(withFrameProtocol(s, FrameProtocol::I2c)) == 0.0);

    // Keys.
    FrameProtocol p = FrameProtocol::Bits;
    for (const FrameProtocol each : {FrameProtocol::Bits, FrameProtocol::Uart, FrameProtocol::I2c, FrameProtocol::Spi, FrameProtocol::Usb})
        CHECK((frameProtocolFromKey(frameProtocolKey(each), p) && p == each));
    CHECK(!frameProtocolFromKey("can", p));
    CHECK(frameProtocolKey(FrameProtocol::I2c) == "i2c");
    CHECK(frameFormatKey(FrameFormat::Decimal) == "dec");
    CHECK(lineCodeKey(LineCode::Manchester) == "manchester");
    CHECK(frameParityKey(FrameParity::Odd) == "odd");
    CHECK(i2cTransferKey(I2cTransfer::WriteRead) == "write-read");
    CHECK(usbPacketKey(UsbPacket::SetupTransaction) == "setup-transaction");

    // Written and read back.
    s = FrameSpec{};
    s.protocol = FrameProtocol::Usb;
    s.data = "0x80 0x06";
    s.format = FrameFormat::Hex;
    s.lsbFirst = true;
    s.code = LineCode::Rz;
    s.dataBits = 7;
    s.parity = FrameParity::Even;
    s.stopBits = 1.5;
    s.i2cAddress = 0x3C;
    s.i2cTransfer = I2cTransfer::WriteRead;
    s.i2cNoAnswer = true;
    s.spiMode = 3;
    s.reply = "0xEF";
    s.usbFullSpeed = true;
    s.usbPacket = UsbPacket::Data1;
    s.usbAddress = 5;
    s.usbEndpoint = 2;
    s.usbFrameNumber = 1234;
    Json j = Json::object();
    frameToJson(s, j);
    CHECK(j["protocol"].toString() == "usb");
    CHECK(j["usbPacket"].toString() == "data1");
    CHECK(j["i2cAddress"].toNumber() == 60.0);
    CHECK((frameFromJson(j) == s));
    // What is missing or wrong: the default; numbers within their bounds.
    CHECK((frameFromJson(Json::object()) == FrameSpec{}));
    CHECK((frameFromJson(Json(3.0)) == FrameSpec{}));
    j = Json::object();
    j["protocol"] = "can";
    j["format"] = "base64";
    j["dataBits"] = 12;
    j["stopBits"] = 1.4;
    j["i2cAddress"] = 300;
    j["spiMode"] = -2;
    j["usbAddress"] = 1e9;
    j["usbEndpoint"] = 99;
    j["usbFrameNumber"] = 5000;
    j["usbPacket"] = "ping";
    j["data"] = 12;
    const FrameSpec read = frameFromJson(j);
    CHECK(read.protocol == FrameProtocol::Bits);
    CHECK(read.format == FrameFormat::Auto);
    CHECK(read.dataBits == 8);
    CHECK(read.stopBits == 1.5);
    CHECK(read.i2cAddress == 127);
    CHECK(read.spiMode == 0);
    CHECK(read.usbAddress == 127);
    CHECK(read.usbEndpoint == 15);
    CHECK(read.usbFrameNumber == 2047);
    CHECK(read.usbPacket == UsbPacket::Setup);
    CHECK(read.data == "Bonjour");
}
