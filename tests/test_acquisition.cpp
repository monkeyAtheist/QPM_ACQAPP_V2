#include "qacq_test.h"

#include "qacq/acquisition/firmware_example.h"
#include "qacq/acquisition/rate_estimator.h"
#include "qacq/acquisition/recorder.h"
#include "qacq/acquisition/settings.h"
#include "qacq/acquisition/simulator.h"
#include "qacq/acquisition/stream_writer.h"
#include "qacq/acquisition/trigger.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/random.h"
#include "qacq/model/csv.h"
#include "qacq/model/wav.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace qacq;
using namespace qacq::acquisition;

namespace
{
SimulatedSettings threeChannels()
{
    SimulatedSettings s;
    s.channels.clear();
    s.channels.push_back({dsp::Waveform::Sine, 1.5, 7.0, 0.0, 0.25, 0.5, 0.0});
    s.channels.push_back({dsp::Waveform::Square, 2.0, 3.0, 45.0, 0.0, 0.3, 0.0});
    s.channels.push_back({dsp::Waveform::Dc, 3.3, 0.0, 0.0, 0.0, 0.5, 0.0});
    return s;
}

io::SampleBlock decodePieces(io::Decoder &decoder, const std::vector<std::uint8_t> &bytes, std::uint64_t seed)
{
    dsp::Random random(seed);
    io::SampleBlock out;
    std::size_t i = 0;
    while (i < bytes.size())
    {
        const std::size_t n = std::min<std::size_t>(bytes.size() - i, 1 + random.next() % 97);
        decoder.decode(std::span<const std::uint8_t>(bytes.data() + i, n), out);
        i += n;
    }
    return out;
}

double quantise(double v, const io::SampleFormat &f)
{
    std::uint8_t b[8] = {};
    io::writeSample(v, f, b);
    return io::readSample(b, f);
}
} // namespace

// ------------------------------------------------------------------------------------------------ settings
TEST_CASE("Acquisition settings: JSON round trip, hex bytes, validation", "[acquisition]")
{
    AcquisitionSettings s;
    s.name = "Banc « moteur »";
    s.source = SourceKind::Scpi;
    s.serial.port = "COM7";
    s.serial.baudRate = 921600;
    s.serial.parity = Parity::Even;
    s.serial.stopBits = 2;
    s.serial.flow = FlowControl::Hardware;
    s.serial.dataTerminalReady = false;
    s.tcp = {"10.0.0.5", 7000};
    s.udp.port = 4210;
    s.udp.multicastGroup = "239.1.2.3";
    s.udp.bindAddress = "127.0.0.1";
    s.scpi.transport = ScpiTransport::Visa;
    s.scpi.visaResource = "USB0::0x0957::0x1796::MY123::INSTR";
    s.scpi.setupCommands = {"*RST", "CONF:VOLT:DC 10"};
    s.scpi.query = "READ?";
    s.scpi.intervalSeconds = 0.2;
    s.audio = {"Micro USB", 96000, 2, AudioSampleType::Float32};
    s.simulated = threeChannels();
    s.simulated.byteErrorRate = 1e-4;
    s.simulated.seed = 77;
    s.decoder.kind = DecoderKind::FramedBinary;
    s.decoder.frame.sync = {0xA5, 0x5A, 0x01};
    s.decoder.frame.counter = io::CounterField::UInt16;
    s.decoder.frame.sample = {io::SampleType::UInt16, io::ByteOrder::BigEndian, 3.3 / 4096.0, -1.65};
    s.decoder.frame.channels = 4;
    s.decoder.frame.samplesPerChannel = 8;
    s.decoder.frame.checksum = io::Checksum::Crc16Modbus;
    s.decoder.text.decimalComma = true;
    s.decoder.scpi.channels = 2;
    s.declaredSampleRateHz = 2500.0;
    s.channels = {{"U", "V"}, {"I", "A"}};
    const model::Json json = toJson(s);
    AcquisitionSettings back;
    std::string error;
    REQUIRE(fromJson(json, back, &error));
    CHECK(error.empty());
    CHECK(toJson(back).dump() == json.dump());
    CHECK(back.decoder.frame.sync == (std::vector<std::uint8_t>{0xA5, 0x5A, 0x01}));
    CHECK(json["decoder"]["frame"]["sync"].toString() == "A5 5A 01");
    CHECK(back.udp.bindAddress == "127.0.0.1");
    // Text form survives a parse: presets are stored as JSON text.
    const auto reparsed = model::Json::parse(json.dump(2));
    REQUIRE(reparsed.has_value());
    AcquisitionSettings third;
    REQUIRE(fromJson(*reparsed, third));
    CHECK(toJson(third).dump() == json.dump());

    std::vector<std::uint8_t> bytes;
    CHECK(parseHexBytes("0xAA 0x55", bytes));
    CHECK(bytes == (std::vector<std::uint8_t>{0xAA, 0x55}));
    CHECK(parseHexBytes("aa,55;7e", bytes));
    CHECK(bytes.size() == 3);
    CHECK(parseHexBytes("AA55", bytes));
    CHECK(bytes == (std::vector<std::uint8_t>{0xAA, 0x55}));
    CHECK(!parseHexBytes("AG", bytes));
    CHECK(!parseHexBytes("ABC", bytes) || bytes.size() == 2); // three digits: AB then a lone C

    AcquisitionSettings wrong;
    auto bad = model::Json::object();
    bad["serial"]["baudRate"] = "rapide";
    CHECK(!fromJson(bad, wrong, &error));
    CHECK(error.find("baudRate") != std::string::npos);

    AcquisitionSettings v;
    v.source = SourceKind::Serial;
    CHECK(!validateSettings(v).empty()); // no port
    v.serial.port = "COM3";
    CHECK(validateSettings(v).empty());
    v.source = SourceKind::Simulated;
    CHECK(!validateSettings(v).empty()); // no rate
    v.declaredSampleRateHz = 1000.0;
    CHECK(validateSettings(v).empty());
    v.decoder.kind = DecoderKind::FramedBinary;
    v.decoder.frame.sync.clear();
    CHECK(!validateSettings(v).empty());
    v.source = SourceKind::Scpi;
    v.scpi.query.clear();
    CHECK(!validateSettings(v).empty());
}

TEST_CASE("Effective decoder: sound card samples, SCPI answers, factory", "[acquisition]")
{
    AcquisitionSettings s;
    s.source = SourceKind::Audio;
    s.audio.channels = 2;
    s.audio.sampleType = AudioSampleType::Int16;
    DecoderSettings d = effectiveDecoder(s);
    CHECK(d.kind == DecoderKind::RawBinary);
    CHECK(d.rawChannels == 2);
    CHECK(d.raw.scale == 1.0 / 32768.0);
    auto decoder = makeDecoder(d);
    REQUIRE(decoder != nullptr);
    io::SampleBlock out;
    const std::uint8_t pcm[] = {0x00, 0x40, 0x00, 0xC0}; // +16384, -16384
    decoder->decode(pcm, out);
    CHECK(out.channelCount == 2);
    CHECK(out.values == (std::vector<double>{0.5, -0.5}));

    s.source = SourceKind::Scpi;
    CHECK(effectiveDecoder(s).kind == DecoderKind::Scpi);
    s.source = SourceKind::Serial;
    s.decoder.kind = DecoderKind::Scpi;
    CHECK(effectiveDecoder(s).kind == DecoderKind::Text);
    DecoderSettings broken;
    broken.kind = DecoderKind::FramedBinary;
    broken.frame.channels = 0;
    std::string error;
    CHECK(makeDecoder(broken, &error) == nullptr);
    CHECK(!error.empty());
}

// ------------------------------------------------------------------------------------------------ simulator
TEST_CASE("Simulator -> decoder: text, raw and framed outputs give back the waveforms", "[acquisition]")
{
    const double rate = 500.0;
    const std::size_t frames = 2000;
    for (DecoderKind kind : {DecoderKind::Text, DecoderKind::RawBinary, DecoderKind::FramedBinary})
    {
        DecoderSettings out;
        out.kind = kind;
        out.raw = {io::SampleType::Float32, io::ByteOrder::LittleEndian, 1.0, 0.0};
        out.rawChannels = 3;
        out.frame.channels = 3;
        out.frame.samplesPerChannel = 5;
        out.frame.sample = {io::SampleType::Int16, io::ByteOrder::LittleEndian, 0.001, 0.0};
        SimulatedDevice device(threeChannels(), out, rate);
        const auto bytes = device.produce(frames);
        auto decoder = makeDecoder(out);
        REQUIRE(decoder != nullptr);
        const io::SampleBlock block = decodePieces(*decoder, bytes, 3);
        REQUIRE(block.channelCount == 3);
        REQUIRE(block.frameCount() == frames);
        double worst = 0.0;
        for (std::size_t f = 0; f < frames; ++f)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                double expected = device.cleanValue(c, f);
                if (kind == DecoderKind::RawBinary) expected = quantise(expected, out.raw);
                if (kind == DecoderKind::FramedBinary) expected = quantise(expected, out.frame.sample);
                worst = std::max(worst, std::abs(block.values[f * 3 + c] - expected));
            }
        }
        CHECK(worst <= (kind == DecoderKind::Text ? 0.5e-4 + 1e-12 : 1e-12));
        CHECK(decoder->statistics().rejected == 0);
        CHECK(block.discontinuities.empty());
    }
}

TEST_CASE("Simulator defects: damaged bytes and lost frames are caught by the frame decoder", "[acquisition]")
{
    SimulatedSettings sim = threeChannels();
    sim.byteErrorRate = 2e-4;
    sim.frameLossRate = 0.01;
    sim.seed = 5;
    DecoderSettings out;
    out.kind = DecoderKind::FramedBinary;
    out.frame.channels = 3;
    out.frame.counter = io::CounterField::UInt16;
    out.frame.sample = {io::SampleType::Int16, io::ByteOrder::LittleEndian, 0.001, 0.0};
    out.frame.checksum = io::Checksum::Crc16Ccitt;
    SimulatedDevice device(sim, out, 1000.0);
    const auto bytes = device.produce(20000);
    auto decoder = makeDecoder(out);
    const io::SampleBlock block = decodePieces(*decoder, bytes, 8);
    const auto &stats = decoder->statistics();
    std::printf("      20000 frames sent, %.0f lost on the link, %.0f decoded, %.0f lost per the counter, %.0f bad CRC\n",
                static_cast<double>(device.framesLost()), static_cast<double>(block.frameCount()),
                static_cast<double>(stats.lostFrames), static_cast<double>(stats.checksumErrors));
    CHECK(device.framesLost() > 100);
    CHECK(block.frameCount() + stats.lostFrames == 20000);
    CHECK(stats.lostFrames >= device.framesLost());
    CHECK(stats.checksumErrors > 0);
    // Every decoded frame is one the device really sent: walk the clean values, allowing gaps.
    std::uint64_t k = 0;
    bool intact = true;
    for (std::size_t f = 0; f < block.frameCount() && intact; ++f)
    {
        bool found = false;
        for (std::uint64_t probe = k; probe < k + 200 && !found; ++probe)
        {
            bool same = true;
            for (std::size_t c = 0; c < 3; ++c)
                same = same && block.values[f * 3 + c] == quantise(device.cleanValue(c, probe), out.frame.sample);
            if (same)
            {
                found = true;
                k = probe + 1;
            }
        }
        intact = found;
    }
    CHECK(intact);
}

TEST_CASE("Simulated SCPI instrument: identification, measurements, waveform blocks, errors", "[acquisition]")
{
    DecoderSettings out;
    out.kind = DecoderKind::Scpi;
    out.scpi.channels = 1;
    out.scpi.block = {io::SampleType::Int16, io::ByteOrder::LittleEndian, 0.001, 0.0};
    SimulatedSettings sim;
    sim.channels = {{dsp::Waveform::Sine, 1.0, 50.0, 0.0, 0.0, 0.5, 0.0}};
    SimulatedDevice device(sim, out, 10000.0);
    CHECK(device.answer("*idn?").find("Simulateur") != std::string::npos);
    const std::string meas = device.answer("MEAS:VOLT:DC?");
    io::ScpiDecoder decoder(out.scpi);
    io::SampleBlock block;
    decoder.decode({reinterpret_cast<const std::uint8_t *>(meas.data()), meas.size()}, block);
    REQUIRE(block.frameCount() == 1);
    CHECK_NEAR(block.values[0], device.cleanValue(0, 0), 0, 1e-6);
    device.waveformPoints = 400;
    const std::string wave = device.answer(":WAV:DATA?");
    CHECK(wave.rfind("#3800", 0) == 0);
    decoder.decode({reinterpret_cast<const std::uint8_t *>(wave.data()), wave.size()}, block);
    REQUIRE(block.frameCount() == 401);
    CHECK(block.discontinuities == std::vector<std::size_t>{1});
    CHECK_NEAR(block.values[1 + 100], quantise(device.cleanValue(0, 101), out.scpi.block), 0, 1e-12);
    CHECK(device.answer("FOO?").empty());
    CHECK(device.answer("SYST:ERR?").rfind("-113", 0) == 0);
    CHECK(device.answer("SYST:ERR?").rfind("+0", 0) == 0);
    CHECK(device.answer("CONF:VOLT:DC 10").empty());
}

TEST_CASE("Measured sample rate: bursts of a serial link, clock off by 0.1 %", "[acquisition]")
{
    SimulatedSettings sim;
    sim.rateErrorPpm = 1000.0;
    DecoderSettings out;
    out.kind = DecoderKind::Text;
    SimulatedDevice device(sim, out, 1000.0);
    auto decoder = makeDecoder(out);
    RateEstimator estimator(5.0);
    dsp::Random random(4);
    double t = 0.0;
    io::SampleBlock block;
    std::uint64_t total = 0;
    while (t < 8.0)
    {
        const double dt = 0.016 * (0.5 + random.uniform()); // a read every 8 to 24 ms
        t += dt;
        decoder->decode(device.produceFor(dt), block);
        total += block.frameCount();
        block.clear();
        estimator.add(t, total);
    }
    CHECK_NEAR(estimator.rateHz(), 1001.0, 2e-4, 0);
    CHECK(estimator.relativeUncertainty() < 1e-3);
    CHECK(estimator.spanSeconds() <= 5.1);
    RateEstimator fresh;
    CHECK(fresh.rateHz() == 0.0);
}

// ------------------------------------------------------------------------------------------------ recorder
TEST_CASE("Live recorder: blocks, copies across them, snapshot, discontinuities", "[acquisition]")
{
    LiveRecorder recorder(2);
    std::vector<double> chunk;
    std::uint64_t n = 0;
    dsp::Random random(1);
    while (n < 200000)
    {
        const std::size_t frames = std::min<std::uint64_t>(200000 - n, 1 + random.next() % 5000);
        chunk.clear();
        for (std::size_t f = 0; f < frames; ++f)
        {
            chunk.push_back(static_cast<double>(n + f));
            chunk.push_back(-static_cast<double>(n + f));
        }
        if (n == 100000) recorder.markDiscontinuity();
        const std::uint64_t breaks[] = {n + frames / 2};
        recorder.append(chunk, n > 150000 && n < 160000 ? std::span<const std::uint64_t>(breaks) : std::span<const std::uint64_t>());
        n += frames;
    }
    CHECK(recorder.totalFrames() == 200000);
    CHECK(recorder.storedFrames() == 200000);
    CHECK(recorder.byteSize() == 200000u * 2 * 8);
    std::vector<double> out;
    CHECK(recorder.copy(1, 65530, 20, out) == 65530);
    REQUIRE(out.size() == 20);
    CHECK(out[0] == -65530.0);
    CHECK(out[19] == -65549.0);
    recorder.copyLatest(0, 10, out);
    CHECK(out.front() == 199990.0);
    CHECK(recorder.copy(0, 199995, 100, out) == 199995);
    CHECK(out.size() == 5);
    CHECK(recorder.value(0, 131072) == 131072.0);
    CHECK(std::isnan(recorder.value(0, 200000)));
    const auto breaks = recorder.discontinuities();
    CHECK(breaks.size() >= 2);
    CHECK(breaks.front() >= 100000);
    const auto data = recorder.snapshot();
    CHECK(data->frameCount() == 200000);
    CHECK(data->channels[1][123456] == -123456.0);
    CHECK(data->discontinuities.size() == breaks.size());
}

TEST_CASE("Live recorder limits: stop when full, or keep the latest frames", "[acquisition]")
{
    RecorderLimits stop;
    stop.maximumBytes = 100000 * 8; // 100 000 frames of one channel
    LiveRecorder full(1, stop);
    std::vector<double> chunk(30000, 1.0);
    for (int k = 0; k < 5; ++k) full.append(chunk);
    CHECK(full.full());
    CHECK(full.storedFrames() == 100000);
    CHECK(full.refusedFrames() == 50000);

    RecorderLimits seconds;
    seconds.maximumSeconds = 2.0;
    LiveRecorder timed(1, seconds, 1000.0);
    std::vector<double> second(1000, 0.0);
    for (int k = 0; k < 3; ++k) timed.append(second);
    CHECK(timed.storedFrames() == 2000);
    CHECK(timed.full());

    RecorderLimits latest;
    latest.maximumBytes = 100000 * 8;
    latest.keepLatest = true;
    LiveRecorder sliding(1, latest);
    std::vector<double> ramp(10000);
    for (std::uint64_t k = 0; k < 100; ++k)
    {
        for (std::size_t f = 0; f < ramp.size(); ++f) ramp[f] = static_cast<double>(k * 10000 + f);
        sliding.append(ramp);
    }
    CHECK(sliding.totalFrames() == 1000000);
    CHECK(!sliding.full());
    CHECK(sliding.storedFrames() >= 100000);
    CHECK(sliding.storedFrames() < 100000 + LiveRecorder::kBlockSamples);
    CHECK(sliding.firstFrame() % LiveRecorder::kBlockSamples == 0);
    CHECK(sliding.value(0, sliding.firstFrame()) == static_cast<double>(sliding.firstFrame()));
    CHECK(sliding.value(0, 999999) == 999999.0);
    CHECK(sliding.snapshot()->frameCount() == sliding.storedFrames());
}

TEST_CASE("Sample pipe: drops when the consumer is late, marked and counted", "[acquisition]")
{
    SamplePipe pipe(2, 1000); // ring of 2048 values = 1024 frames
    io::SampleBlock block;
    block.channelCount = 2;
    for (int f = 0; f < 800; ++f)
    {
        block.values.push_back(f);
        block.values.push_back(-f);
    }
    block.discontinuities = {100};
    CHECK(pipe.push(block) == 800);
    CHECK(pipe.push(block) == 224); // 1024 - 800 frames of room
    CHECK(pipe.droppedFrames() == 576);
    std::vector<double> values;
    std::vector<std::uint64_t> breaks;
    CHECK(pipe.pull(values, breaks, 500) == 500);
    CHECK(breaks == std::vector<std::uint64_t>{100});
    CHECK(pipe.pull(values, breaks) == 524);
    CHECK(breaks == (std::vector<std::uint64_t>{100, 900})); // second block's break at 800 + 100
    CHECK(values.size() == 2048);
    block.discontinuities.clear();
    CHECK(pipe.push(block) == 800);
    CHECK(pipe.pull(values, breaks) == 800);
    CHECK(breaks.back() == 1024); // after the drop
}

TEST_CASE("Sample pipe between two threads, into the recorder", "[acquisition][pipe]")
{
    const std::size_t channels = 3;
    const std::uint64_t total = 600000;
    SamplePipe pipe(channels, 1 << 14);
    std::thread producer([&] {
        dsp::Random random(9);
        std::uint64_t n = 0;
        io::SampleBlock block;
        block.channelCount = channels;
        while (n < total)
        {
            const std::size_t frames = std::min<std::uint64_t>(total - n, 1 + random.next() % 900);
            block.values.clear();
            block.discontinuities.clear();
            for (std::size_t f = 0; f < frames; ++f)
                for (std::size_t c = 0; c < channels; ++c) block.values.push_back(static_cast<double>(n + f) + 0.25 * static_cast<double>(c));
            pipe.push(block);
            n += frames;
            if (random.next() % 4 == 0) std::this_thread::yield();
        }
    });
    LiveRecorder recorder(channels);
    std::vector<double> values;
    std::vector<std::uint64_t> breaks;
    for (;;)
    {
        values.clear();
        breaks.clear();
        if (pipe.pull(values, breaks) > 0) recorder.append(values, breaks);
        else if (recorder.totalFrames() + pipe.droppedFrames() >= total) break; // everything delivered or dropped
        else std::this_thread::yield();
    }
    producer.join();
    CHECK(recorder.totalFrames() + pipe.droppedFrames() == total);
    // Values never go backwards, a jump only where a discontinuity is marked, channels stay together.
    const auto marks = recorder.discontinuities();
    bool consistent = true;
    double previous = -1.0;
    std::size_t m = 0;
    std::vector<double> c0, c2;
    recorder.copy(0, 0, recorder.storedFrames(), c0);
    recorder.copy(2, 0, recorder.storedFrames(), c2);
    for (std::uint64_t f = 0; f < c0.size(); ++f)
    {
        const bool marked = m < marks.size() && marks[m] == f;
        if (marked) ++m;
        consistent = consistent && c2[f] == c0[f] + 0.5 && (c0[f] == previous + 1.0 || (marked && c0[f] > previous));
        previous = c0[f];
    }
    CHECK(consistent);
    std::printf("      %.0f frames through the pipe, %.0f dropped\n", static_cast<double>(recorder.totalFrames()),
                static_cast<double>(pipe.droppedFrames()));
}

// ------------------------------------------------------------------------------------------------ trigger
TEST_CASE("Trigger: rising edges of a sine, interpolated instant, in any chunking", "[acquisition]")
{
    // 50 Hz at 10 kHz: 200 samples per period, zero crossings at 200 k + 37.3 (phase chosen for that).
    const double rate = 10000.0, f0 = 50.0, offset = 37.3;
    std::vector<double> x(20000);
    for (std::size_t n = 0; n < x.size(); ++n) x[n] = std::sin(dsp::kTwoPi * f0 * (static_cast<double>(n) - offset) / rate);
    TriggerSettings s;
    s.level = 0.0;
    s.edge = TriggerEdge::Rising;
    s.mode = TriggerMode::Normal;
    s.length = 400;
    s.preTrigger = 0.25;
    Trigger whole(s);
    const auto captures = whole.process(x, 0);
    REQUIRE(captures.size() >= 40);
    for (const auto &c : captures)
    {
        const double instant = static_cast<double>(c.triggerFrame) - 1.0 + c.triggerFraction;
        const double k = std::round((instant - offset) / 200.0);
        CHECK_NEAR(instant, offset + 200.0 * k, 0, 1e-3);
        CHECK(c.firstFrame + 100 == c.triggerFrame);
        CHECK(!c.forced);
        CHECK(c.rising);
    }
    // Consecutive captures do not overlap: every other period with a 400-sample window.
    CHECK(captures[1].triggerFrame - captures[0].triggerFrame == 400);

    Trigger pieces(s);
    std::vector<TriggerCapture> again;
    dsp::Random random(2);
    for (std::size_t i = 0; i < x.size();)
    {
        const std::size_t n = std::min<std::size_t>(x.size() - i, 1 + random.next() % 333);
        for (const auto &c : pieces.process(std::span<const double>(x).subspan(i, n), i)) again.push_back(c);
        i += n;
    }
    REQUIRE(again.size() == captures.size());
    for (std::size_t k = 0; k < again.size(); ++k)
    {
        CHECK(again[k].triggerFrame == captures[k].triggerFrame);
        CHECK(again[k].triggerFraction == captures[k].triggerFraction);
    }
}

TEST_CASE("Trigger: hysteresis, falling edge, single and auto modes", "[acquisition]")
{
    // A slow ramp with noise around the level: without hysteresis it triggers on the noise.
    std::vector<double> noisy;
    dsp::Random random(3);
    for (int period = 0; period < 10; ++period)
    {
        for (int n = 0; n < 1000; ++n) noisy.push_back((n < 500 ? -1.0 + n / 250.0 : 3.0 - n / 250.0) + 0.05 * random.normal());
    }
    TriggerSettings s;
    s.level = 0.0;
    s.mode = TriggerMode::Normal;
    s.length = 10;
    s.preTrigger = 0.0;
    Trigger jumpy(s);
    const auto many = jumpy.process(noisy, 0);
    s.hysteresis = 0.3;
    Trigger calm(s);
    const auto clean = calm.process(noisy, 0);
    CHECK(many.size() > clean.size());
    CHECK(clean.size() == 10); // one rising crossing per period

    s.edge = TriggerEdge::Falling;
    Trigger falling(s);
    const auto down = falling.process(noisy, 0);
    CHECK(down.size() >= 9);
    CHECK(!down.front().rising);

    s.edge = TriggerEdge::Rising;
    s.mode = TriggerMode::Single;
    Trigger single(s);
    CHECK(single.process(noisy, 0).size() == 1);
    CHECK(!single.armed());
    CHECK(single.process(noisy, noisy.size()).empty());
    single.arm();
    CHECK(single.process(noisy, 2 * noisy.size()).size() == 1);

    std::vector<double> flat(5000, 0.5);
    TriggerSettings a;
    a.level = 2.0;
    a.mode = TriggerMode::Auto;
    a.length = 1000;
    Trigger automatic(a);
    const auto free = automatic.process(flat, 0);
    CHECK(free.size() == 5);
    CHECK(free.front().forced);
    CHECK(free.front().firstFrame == 0);
}

// ------------------------------------------------------------------------------------------------ files
TEST_CASE("Continuous recording: CSV and WAV read back, an unfinished WAV still opens", "[acquisition]")
{
    const auto dir = std::filesystem::temp_directory_path();
    const std::vector<model::ChannelInfo> channels{{"U", "V"}, {"I", "mA"}};
    std::vector<double> block;
    for (int f = 0; f < 3000; ++f)
    {
        block.push_back(std::sin(0.01 * f));
        block.push_back(0.5 * std::cos(0.01 * f));
    }
    {
        StreamWriter csv;
        StreamOptions o;
        o.decimalComma = true;
        REQUIRE(csv.open(dir / "qacq_stream.csv", channels, 1000.0, o).empty());
        REQUIRE(csv.append(std::span<const double>(block).first(2000)).empty());
        REQUIRE(csv.append(std::span<const double>(block).subspan(2000)).empty());
        CHECK(csv.framesWritten() == 3000);
        CHECK(csv.close().empty());
    }
    model::CsvImportOptions in;
    in.decimalComma = true;
    in.firstColumnIsTime = true;
    const auto csv = model::importCsv(dir / "qacq_stream.csv", in);
    REQUIRE(csv.ok());
    CHECK(csv.signal.frameCount() == 3000);
    CHECK(csv.signal.sampleRateHz == 1000.0);
    CHECK(csv.signal.channels[1].unit == "mA");
    CHECK(csv.signal.data->channels[1][1234] == block[1234 * 2 + 1]);

    StreamWriter wav;
    StreamOptions w;
    w.format = StreamFormat::Wav;
    REQUIRE(wav.open(dir / "qacq_stream.wav", channels, 1000.0, w).empty());
    REQUIRE(wav.append(block).empty());
    // Not closed yet: a copy now is what a crash would leave (sizes unknown in the header).
    std::filesystem::copy_file(dir / "qacq_stream.wav", dir / "qacq_crash.wav", std::filesystem::copy_options::overwrite_existing);
    REQUIRE(wav.close().empty());
    const auto finished = model::importWav(dir / "qacq_stream.wav");
    REQUIRE(finished.ok());
    CHECK(finished.warnings.empty());
    CHECK(finished.signal.frameCount() == 3000);
    CHECK(std::abs(finished.signal.data->channels[0][777] - block[777 * 2]) < 1e-7);
    const auto crashed = model::importWav(dir / "qacq_crash.wav");
    REQUIRE(crashed.ok());
    CHECK(crashed.warnings.size() == 1);
    CHECK(crashed.signal.frameCount() >= 1000); // flushed about once a second of signal
    std::filesystem::remove(dir / "qacq_stream.csv");
    std::filesystem::remove(dir / "qacq_stream.wav");
    std::filesystem::remove(dir / "qacq_crash.wav");
}

TEST_CASE("Firmware examples: frame builder, CRC routine, Arduino sketches", "[acquisition]")
{
    DecoderSettings frames;
    frames.kind = DecoderKind::FramedBinary;
    frames.frame.channels = 3;
    frames.frame.samplesPerChannel = 4;
    const std::string c = firmwareExample(frames, FirmwareTarget::PortableC, 3, 1000.0, 115200);
    CHECK(c.find("#define QACQ_FRAME_SIZE 29") != std::string::npos);
    CHECK(c.find("size_t qacq_build_frame(uint8_t *out, uint16_t counter, const int16_t *samples)") != std::string::npos);
    CHECK(c.find("CRC-16/CCITT-FALSE") != std::string::npos);
    CHECK(c.find("0x1021") != std::string::npos);
    frames.frame.checksum = io::Checksum::Crc32;
    CHECK(firmwareExample(frames, FirmwareTarget::PortableC, 3, 1000.0, 115200).find("0xEDB88320") != std::string::npos);
    const std::string sketch = firmwareExample(frames, FirmwareTarget::Arduino, 3, 1000.0, 230400);
    CHECK(sketch.find("Serial.begin(230400)") != std::string::npos);
    CHECK(sketch.find("Serial.write(frame, n)") != std::string::npos);
    DecoderSettings text;
    text.kind = DecoderKind::Text;
    const std::string println = firmwareExample(text, FirmwareTarget::Arduino, 2, 50.0, 115200);
    CHECK(println.find("Serial.println(\"A0,A1\")") != std::string::npos);
    CHECK(println.find("const unsigned long PERIOD_US = 20000;") != std::string::npos);
}
