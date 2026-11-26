#pragma once

// Digital frames for the generator: data typed (bits, hexadecimal, decimal, text) put on a line as two levels,
// as they are (NRZ, NRZI, Manchester, RZ, with their clock on a second line) or framed as a protocol sends
// them: a serial line (UART), an I2C bus (SDA, SCL), an SPI bus (MOSI, MISO, SCLK, CS), a USB packet (D+, D−).
//
// A frame is built on a grid of ticks, a whole number of them a bit (2; 4 for I2C, whose data changes while its
// clock is low; 1 for USB): one level (0 or 1) a tick for each of its lines, and the fields it is made of (start,
// address, acknowledge, data, checksum, stop), to show how it was built. The generator then reads the level of
// the tick each sample falls in (generation.h: WaveKind::Frame).
//
// Data: groups of bits. Hexadecimal ("0x2831", "28 31"), decimal ("40 49") and text ("Bonjour", UTF-8, with
// \n \r \t \0 \\ \xHH) give bytes, sent in the order written; binary ("0011 0010 100") gives the groups typed
// (one group without spaces). Each group is sent from its left bit (most significant first) or from its right
// one (least significant first). The protocols take bytes: bits typed must then come by eight.
//
// Conventions:
//   NRZ: the level is the bit. NRZI: the level changes for a 1 (from low). Manchester (IEEE 802.3): a 1 is low
//   then high, a 0 high then low. RZ: a 1 is high for the first half of the bit. The clock rises in the middle of
//   each bit.
//   UART: idle high; start bit 0, the data bits (least significant first unless asked otherwise), parity (even:
//   an even number of ones among data and parity), 1, 1.5 or 2 stop bits at 1.
//   I2C: idle both high; start (SDA falls while SCL is high), the address (7 bits) and the direction (0: write),
//   each byte most significant bit first and its acknowledge bit (0; 1 after the last byte read, or when nobody
//   answers the address: the frame then stops), stop (SDA rises while SCL is high). SDA changes in the middle
//   of SCL low. A write followed by a read: the bytes written, a repeated start, the address again (read), the
//   bytes of the reply.
//   SPI: CS low from half a bit before the first bit to half a bit after the last; mode = 2 CPOL + CPHA (CPOL:
//   the level of the clock at rest; CPHA 0: data taken on the first edge of each bit, 1: on the second). MISO
//   carries the reply; the shorter of the two is completed with zeros.
//   USB (low speed 1.5 Mbit/s, full speed 12 Mbit/s): SYNC (KJKJKJKK), PID and its complement, fields least
//   significant bit first, CRC5 (tokens) or CRC16 (data), a 0 stuffed after six 1 in a row, NRZI (a 0 changes
//   the state), end of packet (both lines low for two bits, then J). J is D+ high at full speed, D− high at low
//   speed; idle is J.

#include "qacq/model/json.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
enum class FrameProtocol
{
    Bits, // the data as they are, on a line code
    Uart,
    I2c,
    Spi,
    Usb
};

enum class FrameFormat
{
    Auto, // "0x…": hexadecimal; "0b…" or only 0 and 1: binary; "hex: …", "bin: …", "dec: …", "ascii: …" as
          // said; anything else: text (the quotes around it taken off)
    Binary,
    Hex,
    Decimal,
    Text
};

enum class LineCode
{
    Nrz,
    Nrzi,
    Manchester,
    Rz
};

enum class FrameParity
{
    None,
    Even,
    Odd
};

enum class I2cTransfer
{
    Write,    // the data are the bytes written
    Read,     // the data are the bytes the slave sends
    WriteRead // the data are written, then, after a repeated start, the reply is read
};

enum class UsbPacket
{
    Setup, // tokens: address, endpoint, CRC5
    Out,
    In,
    Sof,   // frame number, CRC5
    Data0, // the data, CRC16
    Data1,
    Ack,   // handshakes: the PID alone
    Nak,
    Stall,
    SetupTransaction // SETUP token, DATA0 with the data (the 8 bytes of a request), ACK
};

inline constexpr std::size_t kMaximumFrameBytes = 4096;

struct FrameSpec
{
    FrameProtocol protocol = FrameProtocol::Bits;
    std::string data = "Bonjour";
    FrameFormat format = FrameFormat::Auto;
    bool lsbFirst = false; // each byte (or group of bits typed) from its least significant bit (I2C: never; USB: always)
    LineCode code = LineCode::Nrz;          // Bits
    int dataBits = 8;                       // UART: 5 to 8
    FrameParity parity = FrameParity::None; // UART
    double stopBits = 1.0;                  // UART: 1, 1.5 or 2
    unsigned i2cAddress = 0x48;             // 7 bits
    I2cTransfer i2cTransfer = I2cTransfer::Write;
    bool i2cNoAnswer = false; // nobody acknowledges the address: the frame stops there
    int spiMode = 0;          // 0 to 3
    std::string reply;        // SPI: what MISO carries; I2C write then read: the bytes read (as `data`, same format)
    bool usbFullSpeed = false;
    UsbPacket usbPacket = UsbPacket::Setup;
    unsigned usbAddress = 0;     // 7 bits
    unsigned usbEndpoint = 0;    // 4 bits
    unsigned usbFrameNumber = 0; // SOF: 11 bits

    friend bool operator==(const FrameSpec &, const FrameSpec &) = default;
};

// What a line of a protocol is for: its data, its clock, a second data line (the reply of an SPI slave, the D− of
// USB), a selection.
enum class FrameLineRole
{
    Data,
    Clock,
    Second,
    Select
};

// The lines of a protocol, in the order of BuiltFrame::levels: Bits "data", "clock"; UART "tx"; I2C "sda", "scl";
// SPI "mosi", "miso", "sclk", "cs"; USB "dp", "dm".
struct FrameLineInfo
{
    std::string_view key;
    std::string_view name; // "Données", "Horloge", "TX", "SDA", "SCL", "MOSI", "MISO", "SCLK", "CS", "D+", "D−"
    FrameLineRole role;
};
std::span<const FrameLineInfo> frameLines(FrameProtocol protocol) noexcept;
// The place of a line among those of its protocol (0 for an unknown key: the first line).
std::size_t frameLineIndex(FrameProtocol protocol, std::string_view key) noexcept;
// The line that plays in `to` the part `key` plays in `from` (the clock stays a clock); the first line of `to`
// when it has none.
std::string_view frameLineFor(FrameProtocol from, std::string_view key, FrameProtocol to) noexcept;

// The data as typed, read.
struct FrameData
{
    std::vector<std::uint8_t> bits;  // as written, the leftmost first (bytes: most significant bit first)
    std::vector<std::size_t> groups; // sizes of the groups, in order (bytes: 8 each)
    std::vector<std::uint8_t> bytes; // when the bits come by eight
    FrameFormat format = FrameFormat::Binary; // as read (never Auto)
    std::string error;               // in French

    bool wholeBytes() const noexcept { return bits.size() % 8 == 0; }
    bool ok() const noexcept { return error.empty(); }
};
// Nothing typed: no bit, no error.
FrameData parseFrameData(std::string_view text, FrameFormat format);

// A piece of a frame: what is shown of it ("S", "01000010", "P1", "CRC5 01000"), what it is ("Départ", "Octet
// 0x42 « B »"), and where it lies on the grid of ticks.
struct FrameField
{
    std::string text;
    std::string reply; // SPI: the bits of MISO over the same ticks
    std::string note;
    std::size_t firstTick = 0;
    std::size_t tickCount = 0;
    bool startsGroup = false; // a separator before it when shown
    bool startsLine = false;  // a new line before it when shown (the next packet of a USB transaction)
};

struct BuiltFrame
{
    std::vector<std::vector<std::uint8_t>> levels; // one line each (frameLines), one level a tick
    std::vector<std::uint8_t> idle;                // of each line, before and after the frame
    std::size_t ticksPerBit = 2;
    std::size_t tickCount = 0;
    std::vector<FrameField> fields;
    std::string legend;  // what the marks of the fields mean
    std::string summary; // "7 octets : 42 6F 6E 6A 6F 75 72", "11 bits"
    std::size_t byteCount = 0;
    FrameFormat format = FrameFormat::Binary; // of the data, as read
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
    double bitCount() const noexcept { return static_cast<double>(tickCount) / static_cast<double>(ticksPerBit); }
};
BuiltFrame buildFrame(const FrameSpec &spec);
// The fields in a row: "St 01000010 Sp │ St 11110110 Sp"; beyond `maximumGroups` groups, the first ones, "…" and
// the last one. SPI with a reply: two rows, "MOSI  …" and "MISO  …".
std::string frameConstruction(const BuiltFrame &frame, std::size_t maximumGroups = 24);
// "UART 8N1", "Bits NRZ", "I2C, écriture à 0x48", "SPI mode 0", "USB bas débit, jeton SETUP".
std::string frameTitle(const FrameSpec &spec);
// The same data in another protocol: its usual bit order (UART and USB least significant bit first, the others
// most significant first).
FrameSpec withFrameProtocol(const FrameSpec &spec, FrameProtocol protocol);
// The bit rate of the protocol when it has one: 1 500 000 or 12 000 000 for USB; 0 for the others (any).
double fixedFrameRate(const FrameSpec &spec) noexcept;

// CRC of USB: 5 bits over the bits of a token (x^5 + x^2 + 1; `value` from its bit 0, the first sent), 16 bits
// over the bytes of a data packet (x^16 + x^15 + x^2 + 1); both start from all ones and are sent complemented,
// from bit 0 of the value given here.
unsigned usbCrc5(unsigned value, int bitCount) noexcept;
unsigned usbCrc16(std::span<const std::uint8_t> bytes) noexcept;

// Keys of the recipes: "bits", "uart", "i2c", "spi", "usb"; "auto", "bin", "hex", "dec", "text"; "nrz", "nrzi",
// "manchester", "rz"; "none", "even", "odd"; "write", "read", "write-read"; "setup", "out", "in", "sof", "data0",
// "data1", "ack", "nak", "stall", "setup-transaction".
std::string_view frameProtocolKey(FrameProtocol protocol) noexcept;
bool frameProtocolFromKey(std::string_view key, FrameProtocol &protocol) noexcept;
std::string_view frameFormatKey(FrameFormat format) noexcept;
std::string_view lineCodeKey(LineCode code) noexcept;
std::string_view frameParityKey(FrameParity parity) noexcept;
std::string_view i2cTransferKey(I2cTransfer transfer) noexcept;
std::string_view usbPacketKey(UsbPacket packet) noexcept;
// Into the members of a component of the generator ("protocol", "data", "format", "lsbFirst", "code", "dataBits",
// "parity", "stopBits", "i2cAddress", "i2cTransfer", "i2cNoAnswer", "spiMode", "reply", "usbFullSpeed",
// "usbPacket", "usbAddress", "usbEndpoint", "usbFrameNumber"), and back (what is missing or wrong keeps the
// default; numbers are brought within their bounds).
void frameToJson(const FrameSpec &spec, model::Json &json);
FrameSpec frameFromJson(const model::Json &json);
} // namespace qacq::analysis
