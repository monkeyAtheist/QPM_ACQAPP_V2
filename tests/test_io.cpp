#include "qacq_test.h"

#include "qacq/dsp/random.h"
#include "qacq/io/binary_decoder.h"
#include "qacq/io/crc.h"
#include "qacq/io/scpi.h"
#include "qacq/io/spsc_ring.h"
#include "qacq/io/text_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace qacq::io;
using namespace std::string_view_literals;

namespace
{
std::span<const std::uint8_t> asBytes(std::string_view s)
{
    return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}

// Feeds a stream to a decoder in pieces of random length (1..maxPiece bytes).
SampleBlock decodeInPieces(Decoder &decoder, std::span<const std::uint8_t> stream, std::size_t maxPiece,
                           std::uint64_t seed)
{
    qacq::dsp::Random random(seed);
    SampleBlock out;
    std::size_t i = 0;
    while (i < stream.size())
    {
        const std::size_t piece = std::min<std::size_t>(stream.size() - i, 1 + random.next() % maxPiece);
        decoder.decode(stream.subspan(i, piece), out);
        i += piece;
    }
    return out;
}

const std::string_view kCheck = "123456789";
} // namespace

// ------------------------------------------------------------------------------------------------ CRC
TEST_CASE("CRC presets give the catalogue check values", "[crc]")
{
    CHECK(Crc(kCrc8Smbus).compute(asBytes(kCheck)) == 0xF4u);
    CHECK(Crc(kCrc8Maxim).compute(asBytes(kCheck)) == 0xA1u);
    CHECK(Crc(kCrc16CcittFalse).compute(asBytes(kCheck)) == 0x29B1u);
    CHECK(Crc(kCrc16Xmodem).compute(asBytes(kCheck)) == 0x31C3u);
    CHECK(Crc(kCrc16Modbus).compute(asBytes(kCheck)) == 0x4B37u);
    CHECK(Crc(kCrc32).compute(asBytes(kCheck)) == 0xCBF43926u);
}

TEST_CASE("Generic CRC engine: odd widths and both bit orders", "[crc]")
{
    CHECK(Crc({16, 0x8005, 0x0000, true, true, 0x0000}).compute(asBytes(kCheck)) == 0xBB3Du);         // CRC-16/ARC
    CHECK(Crc({32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0}).compute(asBytes(kCheck)) == 0x0376E6E7u); // CRC-32/MPEG-2
    CHECK(Crc({24, 0x864CFB, 0xB704CE, false, false, 0}).compute(asBytes(kCheck)) == 0x21CF02u);      // CRC-24/OPENPGP
    CHECK(Crc({7, 0x09, 0x00, false, false, 0x00}).compute(asBytes(kCheck)) == 0x75u);                // CRC-7/MMC
    CHECK(Crc({5, 0x05, 0x1F, true, true, 0x1F}).compute(asBytes(kCheck)) == 0x19u);                  // CRC-5/USB
    CHECK(Crc({3, 0x3, 0x7, true, true, 0x0}).compute(asBytes(kCheck)) == 0x6u);                      // CRC-3/ROHC
}

TEST_CASE("CRC: incremental updates, simple checksums", "[crc]")
{
    Crc crc(kCrc32);
    crc.reset();
    crc.update(asBytes("1234"));
    crc.update(asBytes(""));
    crc.update(asBytes("56789"));
    CHECK(crc.value() == 0xCBF43926u);
    CHECK(computeChecksum(Checksum::Sum8, asBytes(kCheck)) == 0xDDu);
    CHECK(computeChecksum(Checksum::Xor8, asBytes(kCheck)) == 0x31u);
    CHECK(computeChecksum(Checksum::Crc16Modbus, asBytes(kCheck)) == 0x4B37u);
    CHECK(checksumSize(Checksum::Crc32) == 4);
    CHECK(checksumSize(Checksum::None) == 0);
}

// ------------------------------------------------------------------------------------------------ text
TEST_CASE("Text: columns, line endings, signs, exponents, any cut of the stream", "[text]")
{
    const std::string stream = "1.5,2\r\n-3e2;+4.25\n\n5\t6\r7   8\n# comment\nnan,inf\n";
    TextDecoder whole;
    SampleBlock ref;
    whole.decode(asBytes(stream), ref);
    REQUIRE(ref.channelCount == 2);
    REQUIRE(ref.frameCount() == 5);
    CHECK(ref.values[0] == 1.5);
    CHECK(ref.values[2] == -300.0);
    CHECK(ref.values[3] == 4.25);
    CHECK(ref.values[6] == 7.0);
    CHECK(std::isnan(ref.values[8]));
    CHECK(std::isinf(ref.values[9]));
    CHECK(whole.statistics().comments == 1);
    CHECK(whole.statistics().rejected == 0);
    CHECK(ref.discontinuities.empty());
    for (std::uint64_t seed = 1; seed <= 20; ++seed)
    {
        TextDecoder pieces;
        const SampleBlock out = decodeInPieces(pieces, asBytes(stream), 1 + seed % 7, seed);
        CHECK(out.values.size() == ref.values.size());
        CHECK(std::equal(out.values.begin(), out.values.begin() + 8, ref.values.begin()));
    }
}

TEST_CASE("Text: decimal comma, labels, header line", "[text]")
{
    TextFormat comma;
    comma.decimalComma = true;
    TextDecoder decoder(comma);
    SampleBlock out;
    decoder.decode(asBytes("1,5;2,25\n-0,5 3\n"), out);
    REQUIRE(out.frameCount() == 2);
    CHECK(out.values[0] == 1.5);
    CHECK(out.values[1] == 2.25);
    CHECK(out.values[2] == -0.5);

    TextDecoder labels;
    SampleBlock l;
    labels.decode(asBytes("temp:21.5,hum:40\ntemp:21.6,hum:41\n"), l);
    REQUIRE(l.frameCount() == 2);
    REQUIRE(l.channelNames.size() == 2);
    CHECK(l.channelNames[0] == "temp");
    CHECK(l.channelNames[1] == "hum");
    CHECK(l.values[3] == 41.0);

    TextDecoder header;
    SampleBlock h;
    header.decode(asBytes("t;ch1;ch2\n0;1;2\n1;3;4\n"), h);
    CHECK(h.channelCount == 3);
    REQUIRE(h.channelNames.size() == 3);
    CHECK(h.channelNames[2] == "ch2");
    CHECK(h.frameCount() == 2);
    CHECK(header.statistics().comments == 1);
}

TEST_CASE("Text: wrong lines are rejected and break the trace", "[text]")
{
    TextDecoder decoder;
    SampleBlock out;
    decoder.decode(asBytes("1,2\n3,4\n5\n6,7,8\noops,9\n10,11\n12,13\n"), out);
    CHECK(out.frameCount() == 4);
    CHECK(decoder.statistics().rejected == 3);
    REQUIRE(out.discontinuities.size() == 1);
    CHECK(out.discontinuities[0] == 2); // before "10,11"
    CHECK(out.values[4] == 10.0);

    TextFormat fixed;
    fixed.channelCount = 3;
    TextDecoder three(fixed);
    SampleBlock t;
    three.decode(asBytes("1,2\n1,2,3\n"), t);
    CHECK(t.frameCount() == 1);
    CHECK(three.statistics().rejected == 1);
}

TEST_CASE("Text: memory stays bounded without newlines, over-long lines are dropped", "[text]")
{
    TextFormat format;
    format.maximumLineLength = 32;
    TextDecoder decoder(format);
    SampleBlock out;
    const std::string garbage(100000, '7');
    for (int i = 0; i < 10; ++i)
    {
        decoder.decode(asBytes(garbage), out);
        CHECK(decoder.pendingBytes() <= 32);
    }
    decoder.decode(asBytes("777\n1,2\n"), out);
    CHECK(out.frameCount() == 1);
    CHECK(out.values[0] == 1.0);
    CHECK(decoder.statistics().overflows == 1);
    decoder.decode(asBytes(std::string(40, '1') + "\n3,4\n"), out);
    CHECK(decoder.statistics().overflows == 2);
    CHECK(out.frameCount() == 2);
}

TEST_CASE("Text: decoding time is linear, whatever the block size (audit: 57 to 650 ms/MB)", "[text][audit]")
{
    std::string stream;
    for (int i = 0; i < 200000; ++i) stream += std::to_string(i) + ",-1.25e-3," + std::to_string(i % 1000) + "\r\n";
    const double megabytes = static_cast<double>(stream.size()) / 1e6;
    auto run = [&](std::size_t block) {
        TextDecoder decoder;
        SampleBlock out;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < stream.size(); i += block)
        {
            decoder.decode(asBytes(std::string_view(stream).substr(i, block)), out);
            if (out.values.size() > 300000) out.clear(); // the consumer takes the frames away
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(decoder.statistics().frames == 200000);
        return ms / megabytes;
    };
    const double small = run(64), large = run(1 << 20);
    std::printf("      %.1f MB: %.1f ms/MB in 64-byte blocks, %.1f ms/MB in 1 MB blocks\n", megabytes, small, large);
    CHECK(small < 4.0 * large + 5.0);
    CHECK(large < 4.0 * small + 5.0);
}

// ------------------------------------------------------------------------------------------------ binary
TEST_CASE("Binary samples: every type, both byte orders, scale and saturation", "[binary]")
{
    const std::uint8_t le16[] = {0x34, 0x12}, be16[] = {0x12, 0x34}, neg[] = {0xFE, 0xFF};
    CHECK(readSample(le16, {SampleType::UInt16, ByteOrder::LittleEndian}) == 4660.0);
    CHECK(readSample(be16, {SampleType::UInt16, ByteOrder::BigEndian}) == 4660.0);
    CHECK(readSample(neg, {SampleType::Int16, ByteOrder::LittleEndian}) == -2.0);
    const std::uint8_t f32[] = {0x00, 0x00, 0xC0, 0x3F};
    CHECK(readSample(f32, {SampleType::Float32, ByteOrder::LittleEndian}) == 1.5);
    // 12-bit ADC on 3.3 V.
    const SampleFormat adc{SampleType::UInt16, ByteOrder::LittleEndian, 3.3 / 4096.0, 0.0};
    const std::uint8_t full[] = {0xFF, 0x0F};
    CHECK_NEAR(readSample(full, adc), 3.3 * 4095.0 / 4096.0, 0, 1e-15);

    for (SampleType type : {SampleType::Int8, SampleType::UInt8, SampleType::Int16, SampleType::UInt16, SampleType::Int32,
                            SampleType::UInt32, SampleType::Float32, SampleType::Float64})
    {
        for (ByteOrder order : {ByteOrder::LittleEndian, ByteOrder::BigEndian})
        {
            const SampleFormat format{type, order, 0.5, 1.0};
            std::uint8_t bytes[8] = {};
            writeSample(43.0, format, bytes); // raw 84
            CHECK(readSample(bytes, format) == 43.0);
        }
    }
    std::uint8_t b[2] = {};
    writeSample(1e9, {SampleType::Int16}, b);
    CHECK(readSample(b, {SampleType::Int16}) == 32767.0);
    writeSample(-1e9, {SampleType::Int16}, b);
    CHECK(readSample(b, {SampleType::Int16}) == -32768.0);
    writeSample(-5.0, {SampleType::UInt16}, b);
    CHECK(readSample(b, {SampleType::UInt16}) == 0.0);
}

TEST_CASE("Raw binary stream: interleaved channels, any cut", "[binary]")
{
    std::vector<std::uint8_t> stream;
    const SampleFormat format{SampleType::Int16, ByteOrder::LittleEndian, 1.0, 0.0};
    for (int i = 0; i < 3000; ++i)
    {
        std::uint8_t s[2];
        writeSample(i % 2 ? -i : i, format, s);
        stream.insert(stream.end(), s, s + 2);
    }
    RawBinaryDecoder decoder(format, 2);
    const SampleBlock out = decodeInPieces(decoder, stream, 13, 4);
    REQUIRE(out.frameCount() == 1500);
    CHECK(out.values[0] == 0.0);
    CHECK(out.values[1] == -1.0);
    CHECK(out.values[2999] == -2999.0);
    CHECK(decoder.pendingBytes() == 0);
}

namespace
{
FrameFormat testFrameFormat()
{
    FrameFormat f;
    f.sync = {0xAA, 0x55};
    f.counter = CounterField::UInt8;
    f.sample = {SampleType::Int16, ByteOrder::LittleEndian, 0.001, 0.0};
    f.channels = 3;
    f.samplesPerChannel = 4;
    f.checksum = Checksum::Crc16Ccitt;
    return f;
}

// Frame k carries the values k + c / 10 + s / 100 (mV resolution). Frames listed in skip are not sent.
std::vector<std::uint8_t> frameStream(const FrameFormat &format, std::size_t frames, std::vector<std::size_t> skip = {})
{
    std::vector<std::uint8_t> stream;
    for (std::size_t k = 0; k < frames; ++k)
    {
        if (std::find(skip.begin(), skip.end(), k) != skip.end()) continue;
        std::vector<double> v;
        for (std::size_t s = 0; s < format.samplesPerChannel; ++s)
            for (std::size_t c = 0; c < format.channels; ++c)
                v.push_back(static_cast<double>(k % 30) + 0.1 * static_cast<double>(c) + 0.01 * static_cast<double>(s));
        const auto frame = encodeFrame(format, static_cast<std::uint32_t>(k), v);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}
} // namespace

TEST_CASE("Framed binary: clean stream in random pieces, counter wrapping", "[binary]")
{
    const FrameFormat format = testFrameFormat();
    CHECK(validateFrameFormat(format).empty());
    CHECK(frameSize(format) == 2 + 1 + 24 + 2);
    const auto stream = frameStream(format, 600); // counter wraps twice
    FramedBinaryDecoder decoder(format);
    const SampleBlock out = decodeInPieces(decoder, stream, 50, 7);
    CHECK(out.channelCount == 3);
    REQUIRE(out.frameCount() == 600 * 4);
    CHECK(decoder.statistics().lostFrames == 0);
    CHECK(decoder.statistics().checksumErrors == 0);
    CHECK(decoder.statistics().skippedBytes == 0);
    CHECK(out.discontinuities.empty());
    // Frame 257, sample 2, channel 1: 257 % 30 + 0.1 + 0.02.
    CHECK_NEAR(out.values[(257 * 4 + 2) * 3 + 1], 17.12, 0, 1e-12);
}

TEST_CASE("Framed binary: corruption, lost bytes, garbage and lost frames are survived", "[binary]")
{
    const FrameFormat format = testFrameFormat();
    const std::size_t size = frameSize(format);
    auto stream = frameStream(format, 100, {50, 51, 52}); // three frames never sent
    // Frame 10: one payload bit flipped -> bad CRC. Frame 20: 5 bytes lost. Before frame 30: 37 bytes of
    // garbage holding fake sync patterns.
    stream[10 * size + 7] ^= 0x10;
    stream.erase(stream.begin() + static_cast<std::ptrdiff_t>(20 * size + 3), stream.begin() + static_cast<std::ptrdiff_t>(20 * size + 8));
    std::vector<std::uint8_t> garbage(37, 0x00);
    for (std::size_t i = 0; i < garbage.size(); i += 6)
    {
        garbage[i] = 0xAA;
        if (i + 1 < garbage.size()) garbage[i + 1] = 0x55;
    }
    const std::size_t at30 = 30 * size - 5;
    stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(at30), garbage.begin(), garbage.end());

    FramedBinaryDecoder decoder(format);
    const SampleBlock out = decodeInPieces(decoder, stream, 64, 3);
    const auto &st = decoder.statistics();
    CHECK(out.frameCount() == (100 - 3 - 2) * 4); // frames 10 and 20 lost to corruption
    CHECK(st.lostFrames == 5);                     // 10, 20, 50, 51, 52 (counter)
    CHECK(st.checksumErrors >= 2);
    CHECK(out.discontinuities.size() == 3);
    // Every decoded frame is intact: values agree with their counter-derived pattern.
    bool intact = true;
    for (std::size_t f = 0; f < out.frameCount(); f += 4)
    {
        const double base = out.values[f * 3];
        for (std::size_t s = 0; s < 4; ++s)
            for (std::size_t c = 0; c < 3; ++c)
                intact = intact && std::abs(out.values[(f + s) * 3 + c] - (base + 0.1 * static_cast<double>(c) + 0.01 * static_cast<double>(s))) < 1e-9;
    }
    CHECK(intact);
}

TEST_CASE("Framed binary without checksum: a sync-like value inside the data cannot lock the decoder", "[binary]")
{
    FrameFormat format;
    format.sync = {0xAA, 0x55};
    format.counter = CounterField::None;
    format.sample = {SampleType::UInt16, ByteOrder::BigEndian, 1.0, 0.0};
    format.channels = 2;
    format.checksum = Checksum::None;
    std::vector<std::uint8_t> stream;
    for (std::uint32_t k = 0; k < 200; ++k)
    {
        // Every third frame carries a value that looks like the sync pattern.
        const double values[] = {k % 3 == 0 ? static_cast<double>(0xAA55) : static_cast<double>(7 * k), static_cast<double>(k)};
        const auto frame = encodeFrame(format, 0, values);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    // Start in the middle of frame 0, right on its fake sync.
    FramedBinaryDecoder decoder(format);
    const auto tail = std::span<const std::uint8_t>(stream).subspan(2);
    const SampleBlock out = decodeInPieces(decoder, tail, 9, 5);
    REQUIRE(out.frameCount() == 199); // frames 1..199, aligned on the real syncs
    bool aligned = true;
    for (std::size_t f = 0; f < out.frameCount(); ++f) aligned = aligned && out.values[2 * f + 1] == static_cast<double>(f + 1);
    CHECK(aligned);
    CHECK(decoder.statistics().skippedBytes == 4);
}

TEST_CASE("Framed binary: every checksum, big-endian counters, invalid formats", "[binary]")
{
    for (Checksum checksum : {Checksum::None, Checksum::Sum8, Checksum::Xor8, Checksum::Crc8, Checksum::Crc8Maxim,
                              Checksum::Crc16Ccitt, Checksum::Crc16Xmodem, Checksum::Crc16Modbus, Checksum::Crc32})
    {
        FrameFormat format = testFrameFormat();
        format.checksum = checksum;
        format.counter = CounterField::UInt16;
        format.sample.order = ByteOrder::BigEndian;
        format.checksumCoversSync = checksum == Checksum::Crc32;
        const auto stream = frameStream(format, 40);
        FramedBinaryDecoder decoder(format);
        const SampleBlock out = decodeInPieces(decoder, stream, 17, 9);
        CHECK(out.frameCount() == 40 * 4);
        CHECK(decoder.statistics().lostFrames == 0);
    }
    FrameFormat bad = testFrameFormat();
    bad.sync.clear();
    CHECK(!validateFrameFormat(bad).empty());
    bad = testFrameFormat();
    bad.channels = 0;
    CHECK(!validateFrameFormat(bad).empty());
    FramedBinaryDecoder refused(bad);
    SampleBlock out;
    refused.decode(asBytes("abc"), out);
    CHECK(!refused.formatError().empty());
    CHECK(out.values.empty());
}

TEST_CASE("Framed binary: random corruption never yields a wrong frame (fuzz)", "[binary]")
{
    const FrameFormat format = testFrameFormat();
    const auto clean = frameStream(format, 300);
    std::uint64_t decodedTotal = 0;
    for (std::uint64_t seed = 1; seed <= 30; ++seed)
    {
        qacq::dsp::Random random(seed);
        std::vector<std::uint8_t> stream;
        for (std::size_t i = 0; i < clean.size(); ++i)
        {
            const std::uint64_t r = random.next() % 1000;
            if (r < 3) continue;                                                     // byte lost
            if (r < 6) stream.push_back(static_cast<std::uint8_t>(random.next()));   // byte added
            stream.push_back(r < 10 ? static_cast<std::uint8_t>(random.next()) : clean[i]); // byte changed
        }
        FramedBinaryDecoder decoder(format);
        const SampleBlock out = decodeInPieces(decoder, stream, 100, seed + 100);
        CHECK(out.frameCount() <= 300 * 4);
        CHECK(decoder.pendingBytes() < 2 * frameSize(format));
        bool intact = true;
        for (std::size_t f = 0; f < out.frameCount(); f += 4)
        {
            const double base = out.values[f * 3];
            for (std::size_t k = 0; k < 12; ++k)
                intact = intact && std::abs(out.values[f * 3 + k] - (base + 0.1 * static_cast<double>(k % 3) + 0.01 * static_cast<double>(k / 3))) < 1e-9;
        }
        CHECK(intact);
        decodedTotal += out.frameCount() / 4;
    }
    // About 1 % of the bytes damaged, 29-byte frames: roughly three frames in four get through.
    std::printf("      %.0f frames of 9000 decoded intact\n", static_cast<double>(decodedTotal));
    CHECK(decodedTotal > 5000);
}

TEST_CASE("Text and SCPI decoders on random bytes: no crash, bounded memory (fuzz)", "[text][scpi]")
{
    qacq::dsp::Random random(77);
    std::vector<std::uint8_t> noise(200000);
    for (auto &b : noise)
    {
        const std::uint64_t r = random.next() % 16;
        b = r < 6 ? static_cast<std::uint8_t>("0123456789,.-e\n#"[random.next() % 16]) : static_cast<std::uint8_t>(random.next());
    }
    TextFormat format;
    format.maximumLineLength = 256;
    TextDecoder text(format);
    ScpiFormat scpiFormat;
    scpiFormat.maximumAnswerBytes = 4096;
    ScpiDecoder scpi(scpiFormat);
    SampleBlock a, b;
    for (std::size_t i = 0; i < noise.size(); i += 333)
    {
        const auto piece = std::span<const std::uint8_t>(noise).subspan(i, std::min<std::size_t>(333, noise.size() - i));
        text.decode(piece, a);
        scpi.decode(piece, b);
        CHECK(text.pendingBytes() <= 256);
        CHECK(scpi.pendingBytes() <= 4096 + 16);
    }
    CHECK(text.statistics().bytes == noise.size());
    CHECK(scpi.statistics().bytes == noise.size());
}

// ------------------------------------------------------------------------------------------------ SCPI
TEST_CASE("SCPI: IEEE 488.2 block headers and error answers", "[scpi]")
{
    using S = BlockHeader::Status;
    CHECK(parseBlockHeader(asBytes("#"), 1000).status == S::Incomplete);
    CHECK(parseBlockHeader(asBytes("#3"), 1000).status == S::Incomplete);
    CHECK(parseBlockHeader(asBytes("#31"), 1000).status == S::Incomplete);
    const BlockHeader h = parseBlockHeader(asBytes("#3123xyz"), 1000);
    CHECK(h.status == S::Valid);
    CHECK(h.headerSize == 5);
    CHECK(h.dataSize == 123);
    CHECK(parseBlockHeader(asBytes("#0abc"), 1000).indefinite);
    CHECK(parseBlockHeader(asBytes("#A12"), 1000).status == S::Invalid);
    CHECK(parseBlockHeader(asBytes("#2x1"), 1000).status == S::Invalid);
    CHECK(parseBlockHeader(asBytes("#41200"), 1000).status == S::Invalid); // larger than allowed
    CHECK(blockHeader(1200) == "#41200");
    const auto e = parseScpiError("-113,\"Undefined header\"\n");
    REQUIRE(e.has_value());
    CHECK(e->code == -113);
    CHECK(e->message == "Undefined header");
    CHECK(parseScpiError("+0,\"No error\"")->code == 0);
    CHECK(!parseScpiError("hello").has_value());
}

TEST_CASE("SCPI answers: polled values, ASCII and binary waveforms, text answers", "[scpi]")
{
    ScpiFormat format;
    format.block = {SampleType::Int16, ByteOrder::BigEndian, 0.5, 0.0};
    std::string stream = "+1.0E+00\n+2.0E+00\r\n9.91E37\n";       // three polled measurements
    stream += "RIGOL TECHNOLOGIES,DS1054Z,DS1ZA0000,00.04.04\n";  // *IDN?
    stream += "1,2,3,4\n";                                        // ASCII waveform: a record
    std::string block = blockHeader(6);
    const std::uint8_t data[] = {0x00, 0x02, 0xFF, 0xFE, 0x00, 0x08}; // 1, -1, 4 after scale
    block.append(reinterpret_cast<const char *>(data), sizeof data);
    stream += block + "\n";
    stream += "-9.9E37\n";
    ScpiDecoder whole(format);
    SampleBlock ref;
    whole.decode(asBytes(stream), ref);
    REQUIRE(ref.frameCount() == 3 + 4 + 3 + 1);
    CHECK(ref.values[0] == 1.0);
    CHECK(std::isnan(ref.values[2]));
    CHECK(ref.values[3] == 1.0);
    CHECK(ref.values[7] == 1.0);
    CHECK(ref.values[8] == -1.0);
    CHECK(ref.values[9] == 4.0);
    CHECK(std::isinf(ref.values[10]));
    CHECK(ref.values[10] < 0.0);
    CHECK(whole.statistics().comments == 1);
    CHECK(whole.answers() == 7);
    // Breaks before the ASCII waveform, before the block, and after it (back to single values).
    CHECK((ref.discontinuities == std::vector<std::size_t>{3, 7, 10}));
    for (std::uint64_t seed = 1; seed <= 10; ++seed)
    {
        ScpiDecoder pieces(format);
        const SampleBlock out = decodeInPieces(pieces, asBytes(stream), 1 + seed, seed);
        CHECK(out.frameCount() == ref.frameCount());
        CHECK(out.discontinuities == ref.discontinuities);
        CHECK(out.values[9] == 4.0);
    }
}

TEST_CASE("SCPI: multi-channel answers, odd block sizes, huge headers refused", "[scpi]")
{
    ScpiFormat format;
    format.channels = 3;
    ScpiDecoder decoder(format);
    SampleBlock out;
    decoder.decode(asBytes("1.1,2.2,3.3\n4,5\n"), out); // second answer: not a multiple of 3
    CHECK(out.frameCount() == 1);
    CHECK(decoder.statistics().rejected == 1);

    ScpiFormat bytes;
    bytes.block = {SampleType::Int16, ByteOrder::LittleEndian, 1.0, 0.0};
    bytes.maximumAnswerBytes = 1000;
    ScpiDecoder odd(bytes);
    SampleBlock o;
    odd.decode(asBytes("#15\x01\x00\x02\x00\x03\n"sv), o); // 5 bytes: two int16 and a lone byte
    CHECK(o.frameCount() == 2);
    CHECK(odd.statistics().rejected == 1);
    odd.decode(asBytes("#9000002000"), o); // 2000 bytes > 1000 allowed
    CHECK(odd.statistics().rejected == 2);
    odd.decode(asBytes("\n7\n"), o);
    CHECK(o.frameCount() == 3);
    CHECK(o.values[2] == 7.0);
}

// ------------------------------------------------------------------------------------------------ ring
TEST_CASE("SPSC ring: capacity, wrap-around, all-or-nothing writes", "[ring]")
{
    SpscRing<double> ring(1000);
    CHECK(ring.capacity() == 1024);
    std::vector<double> in(700), out(700);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<double>(i);
    for (int round = 0; round < 5; ++round)
    {
        CHECK(ring.push(in) == 700);
        CHECK(ring.pop(out) == 700);
        CHECK(out == in);
    }
    CHECK(ring.push(in) == 700);
    CHECK(!ring.pushAll(in)); // 324 free
    CHECK(ring.push(in) == 324);
    CHECK(ring.size() == 1024);
    std::vector<double> all(2000);
    CHECK(ring.pop(all) == 1024);
    CHECK(all[699] == 699.0);
    CHECK(all[700] == 0.0);
    CHECK(ring.empty());
}

TEST_CASE("SPSC ring: one producer and one consumer thread, nothing lost or reordered", "[ring]")
{
    SpscRing<std::uint64_t> ring(4096);
    const std::uint64_t total = 3'000'000;
    std::thread producer([&] {
        qacq::dsp::Random random(1);
        std::vector<std::uint64_t> chunk;
        std::uint64_t next = 0;
        while (next < total)
        {
            const std::uint64_t n = std::min<std::uint64_t>(total - next, 1 + random.next() % 700);
            chunk.resize(n);
            for (std::uint64_t i = 0; i < n; ++i) chunk[i] = next + i;
            std::span<const std::uint64_t> rest(chunk);
            while (!rest.empty())
            {
                const std::size_t written = ring.push(rest);
                rest = rest.subspan(written);
                if (written == 0) std::this_thread::yield();
            }
            next += n;
        }
    });
    std::vector<std::uint64_t> buffer(1000);
    std::uint64_t expected = 0;
    bool ordered = true;
    qacq::dsp::Random random(2);
    while (expected < total)
    {
        const std::size_t got = ring.pop(std::span<std::uint64_t>(buffer).first(1 + random.next() % 1000));
        for (std::size_t i = 0; i < got; ++i) ordered = ordered && buffer[i] == expected++;
        if (got == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(ordered);
    CHECK(expected == total);
    CHECK(ring.empty());
}
