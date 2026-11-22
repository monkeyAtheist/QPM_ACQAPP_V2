#pragma once

// Everything that describes an acquisition, independent of Qt: the source (serial port, TCP, UDP, SCPI
// instrument, sound card, simulator), how its bytes are decoded, the sample rate and the channel names.
// Stored as JSON in the presets and in the provenance of the acquired signal.

#include "qacq/dsp/generator.h"
#include "qacq/io/binary_decoder.h"
#include "qacq/io/decoder.h"
#include "qacq/io/scpi.h"
#include "qacq/io/text_decoder.h"
#include "qacq/model/json.h"
#include "qacq/model/signal.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::acquisition
{
enum class SourceKind
{
    Serial,
    TcpClient,
    Udp,
    Scpi,
    Audio,
    Simulated
};

std::string_view sourceKey(SourceKind kind) noexcept;  // "serial", "tcp", "udp", "scpi", "audio", "simulated"
std::string_view sourceName(SourceKind kind) noexcept; // "Port série"...
bool sourceFromKey(std::string_view key, SourceKind &kind) noexcept;

enum class Parity
{
    None,
    Even,
    Odd,
    Mark,
    Space
};

enum class FlowControl
{
    None,
    Hardware, // RTS/CTS
    Software  // XON/XOFF
};

struct SerialSettings
{
    std::string port;          // "COM3", "/dev/ttyACM0"
    int baudRate = 115200;
    int dataBits = 8;          // 5..8
    Parity parity = Parity::None;
    int stopBits = 1;          // 1 or 2
    FlowControl flow = FlowControl::None;
    bool dataTerminalReady = true; // most Arduino boards reset when DTR rises: kept on by default
};

struct TcpSettings
{
    std::string host = "192.168.1.10";
    int port = 5000;
};

struct UdpSettings
{
    int port = 5000;              // local port the datagrams arrive on
    std::string multicastGroup;   // empty: unicast or broadcast
    std::string bindAddress;      // empty: every interface; "127.0.0.1": this PC only (no firewall prompt)
};

enum class ScpiTransport
{
    Tcp, // raw socket, port 5025 on most instruments
    Visa // USB-TMC, GPIB... through an installed VISA library (later)
};

struct ScpiSettings
{
    ScpiTransport transport = ScpiTransport::Tcp;
    std::string host = "192.168.1.20";
    int port = 5025;
    std::string visaResource; // "USB0::0x0957::0x1796::MY12345678::INSTR"
    std::vector<std::string> setupCommands; // sent once after *IDN?
    std::string query = "MEAS:VOLT:DC?";
    double intervalSeconds = 0.5; // 0: as fast as the instrument answers
    double timeoutSeconds = 3.0;
    bool checkErrors = true; // SYST:ERR? after the setup commands
};

enum class AudioSampleType
{
    Int16,
    Int32,
    Float32
};

struct AudioSettings
{
    std::string device; // empty: the default input
    int sampleRate = 48000;
    int channels = 1;
    AudioSampleType sampleType = AudioSampleType::Int16;
};

struct SimulatedChannel
{
    dsp::Waveform waveform = dsp::Waveform::Sine;
    double amplitude = 1.0;
    double frequencyHz = 5.0;
    double phaseDegrees = 0.0;
    double offset = 0.0;
    double dutyCycle = 0.5;
    double noise = 0.0; // standard deviation of an added white Gaussian noise
};

struct SimulatedSettings
{
    std::vector<SimulatedChannel> channels{SimulatedChannel{}};
    double byteErrorRate = 0.0;    // probability that a byte is damaged (one bit flipped)
    double frameLossRate = 0.0;    // probability that a frame (or a line) is not sent
    double rateErrorPpm = 0.0;     // the simulated board's clock is off by this much
    std::uint64_t seed = 1;
    int textDecimals = 4;          // text output: digits after the point
    bool textLabels = false;       // text output: "Voie1:0.1234,Voie2:..."
};

enum class DecoderKind
{
    Text,
    RawBinary,
    FramedBinary,
    Scpi
};

std::string_view decoderKey(DecoderKind kind) noexcept; // "text", "raw", "frames", "scpi"
std::string_view decoderName(DecoderKind kind) noexcept;

struct DecoderSettings
{
    DecoderKind kind = DecoderKind::Text;
    io::TextFormat text;
    io::SampleFormat raw;
    std::size_t rawChannels = 1;
    io::FrameFormat frame;
    io::ScpiFormat scpi;

    std::size_t channelCount() const noexcept; // 0 for text with an automatic count
};

struct AcquisitionSettings
{
    std::string name = "Acquisition";
    SourceKind source = SourceKind::Serial;
    SerialSettings serial;
    TcpSettings tcp;
    UdpSettings udp;
    ScpiSettings scpi;
    AudioSettings audio;
    SimulatedSettings simulated;
    DecoderSettings decoder;
    double declaredSampleRateHz = 0.0; // 0: measured from the arrival of the frames
    std::vector<model::ChannelInfo> channels; // names and units; missing ones get defaults
};

// The rate the samples come at when it is known before they arrive: the sound card's own (whatever was
// announced for another source and left in the settings), else the one announced (0: measured from their
// arrival).
double announcedRate(const AcquisitionSettings &settings) noexcept;

// The name of the signals acquired unless another is given: "Essai du simulateur", "Enregistrement" (sound
// card), "Acquisition".
std::string_view defaultSignalName(SourceKind kind) noexcept;
// The name of the signals of a source: none, or one left from another source (« Essai du simulateur » for the
// sound card), becomes its own; a name of one's own is kept.
std::string nameForSource(const std::string &name, SourceKind source);

// Empty when the settings can be used, else the reason (in French).
std::string validateSettings(const AcquisitionSettings &settings);

// The decoder the source needs. The sound card always gives raw samples (its own format); SCPI answers
// always go through the SCPI decoder.
DecoderSettings effectiveDecoder(const AcquisitionSettings &settings);
std::unique_ptr<io::Decoder> makeDecoder(const DecoderSettings &settings, std::string *error = nullptr);

model::Json toJson(const AcquisitionSettings &settings);
// Missing members keep their default value; wrong ones are reported.
bool fromJson(const model::Json &json, AcquisitionSettings &settings, std::string *error = nullptr);
model::Json frameFormatToJson(const io::FrameFormat &format);
bool frameFormatFromJson(const model::Json &json, io::FrameFormat &format, std::string *error = nullptr);

// "AA 55" <-> {0xAA, 0x55} (spaces, commas or "0x" accepted on input).
std::string hexBytes(const std::vector<std::uint8_t> &bytes);
bool parseHexBytes(std::string_view text, std::vector<std::uint8_t> &bytes);
} // namespace qacq::acquisition
