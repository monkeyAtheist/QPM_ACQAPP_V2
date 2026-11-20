#include "qacq/acquisition/firmware_example.h"

#include "qacq/io/crc.h"
#include "qacq/model/json.h"

#include <cmath>
#include <string>

namespace qacq::acquisition
{
namespace
{
// "0x" and at least `digits` upper-case hexadecimal digits (no printf: %llX is unsafe with MinGW's msvcrt).
std::string hex(std::uint64_t v, int digits)
{
    static const char table[] = "0123456789ABCDEF";
    std::string s;
    do
    {
        s.insert(s.begin(), table[v & 0xF]);
        v >>= 4;
    } while (v);
    while (static_cast<int>(s.size()) < digits) s.insert(s.begin(), '0');
    return "0x" + s;
}

std::uint32_t reflect(std::uint32_t v, int bits)
{
    std::uint32_t r = 0;
    for (int i = 0; i < bits; ++i)
        if (v & (1u << i)) r |= 1u << (bits - 1 - i);
    return r;
}

const char *cType(io::SampleType t)
{
    switch (t)
    {
        case io::SampleType::Int8: return "int8_t";
        case io::SampleType::UInt8: return "uint8_t";
        case io::SampleType::Int16: return "int16_t";
        case io::SampleType::UInt16: return "uint16_t";
        case io::SampleType::Int32: return "int32_t";
        case io::SampleType::UInt32: return "uint32_t";
        case io::SampleType::Float32: return "float";
        case io::SampleType::Float64: return "double";
    }
    return "int16_t";
}

const char *unsignedOfSize(std::size_t bytes)
{
    return bytes == 1 ? "uint8_t" : bytes == 2 ? "uint16_t" : bytes == 4 ? "uint32_t" : "uint64_t";
}

std::string checksumSpecText(io::Checksum c)
{
    switch (c)
    {
        case io::Checksum::Crc8: return "CRC-8/SMBUS, \"123456789\" -> 0xF4";
        case io::Checksum::Crc8Maxim: return "CRC-8/MAXIM, \"123456789\" -> 0xA1";
        case io::Checksum::Crc16Ccitt: return "CRC-16/CCITT-FALSE, \"123456789\" -> 0x29B1";
        case io::Checksum::Crc16Xmodem: return "CRC-16/XMODEM, \"123456789\" -> 0x31C3";
        case io::Checksum::Crc16Modbus: return "CRC-16/MODBUS, \"123456789\" -> 0x4B37";
        case io::Checksum::Crc32: return "CRC-32, \"123456789\" -> 0xCBF43926";
        case io::Checksum::Sum8: return "somme des octets modulo 256, \"123456789\" -> 0xDD";
        case io::Checksum::Xor8: return "XOR des octets, \"123456789\" -> 0x31";
        case io::Checksum::None: return "aucune";
    }
    return "";
}

io::CrcSpec specOf(io::Checksum c)
{
    switch (c)
    {
        case io::Checksum::Crc8: return io::kCrc8Smbus;
        case io::Checksum::Crc8Maxim: return io::kCrc8Maxim;
        case io::Checksum::Crc16Xmodem: return io::kCrc16Xmodem;
        case io::Checksum::Crc16Modbus: return io::kCrc16Modbus;
        case io::Checksum::Crc32: return io::kCrc32;
        default: return io::kCrc16CcittFalse;
    }
}

// static uintN_t qacq_check(const uint8_t *data, size_t n)
std::string checksumFunction(io::Checksum c)
{
    const std::size_t bytes = io::checksumSize(c);
    if (bytes == 0) return {};
    const std::string t = unsignedOfSize(bytes);
    std::string s = "/* Somme de contrôle : " + checksumSpecText(c) + " */\n";
    s += "static " + t + " qacq_check(const uint8_t *data, size_t n)\n{\n";
    if (c == io::Checksum::Sum8 || c == io::Checksum::Xor8)
    {
        s += "    uint8_t sum = 0;\n    while (n--) sum " + std::string(c == io::Checksum::Sum8 ? "+=" : "^=") + " *data++;\n    return sum;\n}\n";
        return s;
    }
    const io::CrcSpec spec = specOf(c);
    const int w = spec.width;
    const std::uint32_t mask = w >= 32 ? 0xFFFFFFFFu : (1u << w) - 1u;
    if (spec.reflectIn)
    {
        s += "    " + t + " crc = " + hex(reflect(spec.initial & mask, w), w / 4) + ";\n";
        s += "    while (n--) {\n        crc ^= *data++;\n";
        s += "        for (int bit = 0; bit < 8; ++bit)\n";
        s += "            crc = (crc & 1u) ? (" + t + ")((crc >> 1) ^ " + hex(reflect(spec.polynomial & mask, w), w / 4) + "u) : (" + t +
             ")(crc >> 1);\n    }\n";
    }
    else
    {
        s += "    " + t + " crc = " + hex(spec.initial & mask, w / 4) + ";\n";
        s += "    while (n--) {\n        crc ^= (" + t + ")((" + t + ")(*data++) << " + std::to_string(w - 8) + ");\n";
        s += "        for (int bit = 0; bit < 8; ++bit)\n";
        s += "            crc = (crc & " + hex(1ull << (w - 1), w / 4) + "u) ? (" + t + ")((crc << 1) ^ " + hex(spec.polynomial & mask, w / 4) +
             "u) : (" + t + ")(crc << 1);\n    }\n";
    }
    if (spec.finalXor) s += "    return (" + t + ")(crc ^ " + hex(spec.finalXor & mask, w / 4) + "u);\n}\n";
    else s += "    return crc;\n}\n";
    return s;
}

// static size_t qacq_put(uint8_t *out, uintN_t v, size_t bytes): byte order of the format.
std::string putFunction(io::ByteOrder order)
{
    std::string s = "/* Écrit les `bytes` octets de v, " +
                    std::string(order == io::ByteOrder::LittleEndian ? "poids faible en tête (petit boutisme)" : "poids fort en tête (grand boutisme)") +
                    ". */\n";
    s += "static size_t qacq_put(uint8_t *out, uint64_t v, size_t bytes)\n{\n";
    s += "    for (size_t i = 0; i < bytes; ++i)\n";
    if (order == io::ByteOrder::LittleEndian) s += "        out[i] = (uint8_t)(v >> (8 * i));\n";
    else s += "        out[bytes - 1 - i] = (uint8_t)(v >> (8 * i));\n";
    s += "    return bytes;\n}\n";
    return s;
}

std::string sampleExpression(io::SampleType t, const std::string &value)
{
    // The raw bits of the sample, as an unsigned integer.
    switch (t)
    {
        case io::SampleType::Float32: return "qacq_bits32(" + value + ")";
        case io::SampleType::Float64: return "qacq_bits64(" + value + ")";
        case io::SampleType::Int8: return "(uint8_t)" + value;
        case io::SampleType::Int16: return "(uint16_t)" + value;
        case io::SampleType::Int32: return "(uint32_t)" + value;
        default: return "(uint64_t)" + value;
    }
}

std::string floatHelpers(io::SampleType t)
{
    if (t == io::SampleType::Float32)
        return "static uint32_t qacq_bits32(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }\n";
    if (t == io::SampleType::Float64)
        return "static uint64_t qacq_bits64(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }\n";
    return {};
}

std::string frameBuilder(const io::FrameFormat &f)
{
    const std::size_t counterBytes = f.counter == io::CounterField::None ? 0 : f.counter == io::CounterField::UInt8 ? 1 : 2;
    const std::size_t sample = io::sampleSize(f.sample.type);
    const std::size_t check = io::checksumSize(f.checksum);
    std::string s;
    s += "#define QACQ_CHANNELS " + std::to_string(f.channels) + "\n";
    s += "#define QACQ_SAMPLES_PER_CHANNEL " + std::to_string(f.samplesPerChannel) + "\n";
    s += "#define QACQ_FRAME_SIZE " + std::to_string(io::frameSize(f)) + " /* octets */\n\n";
    s += putFunction(f.sample.order) + "\n";
    s += floatHelpers(f.sample.type);
    if (!floatHelpers(f.sample.type).empty()) s += "\n";
    s += checksumFunction(f.checksum);
    if (check) s += "\n";
    s += "/* Construit une trame dans out (QACQ_FRAME_SIZE octets) et renvoie sa longueur.\n";
    s += "   samples : QACQ_SAMPLES_PER_CHANNEL x QACQ_CHANNELS valeurs brutes, échantillon par échantillon\n";
    s += "   (s0v0 s0v1 ... s1v0 ...), dans l'unité du convertisseur : le PC applique l'échelle. */\n";
    s += "size_t qacq_build_frame(uint8_t *out, uint16_t counter, const " + std::string(cType(f.sample.type)) + " *samples)\n{\n";
    s += "    size_t p = 0;\n";
    for (std::uint8_t b : f.sync) s += "    out[p++] = " + hex(b, 2) + ";\n";
    if (counterBytes) s += "    p += qacq_put(out + p, counter, " + std::to_string(counterBytes) + ");\n";
    else s += "    (void)counter;\n";
    s += "    for (size_t i = 0; i < QACQ_CHANNELS * QACQ_SAMPLES_PER_CHANNEL; ++i)\n";
    s += "        p += qacq_put(out + p, " + sampleExpression(f.sample.type, "samples[i]") + ", " + std::to_string(sample) + ");\n";
    if (check)
    {
        const std::size_t from = f.checksumCoversSync ? 0 : f.sync.size();
        s += "    p += qacq_put(out + p, qacq_check(out + " + std::to_string(from) + ", p - " + std::to_string(from) + "), " +
             std::to_string(check) + ");\n";
    }
    s += "    return p;\n}\n";
    return s;
}

std::string header(const DecoderSettings &format, std::size_t channels, double rate)
{
    std::string s = "/* Exemple généré par QTacquisitionApp : " + std::string(decoderName(format.kind)) + ", " +
                    std::to_string(channels) + " voie(s)";
    if (rate > 0.0) s += ", " + model::formatNumber(rate) + " éch./s";
    s += ".\n";
    if (format.kind == DecoderKind::FramedBinary)
    {
        const io::FrameFormat &f = format.frame;
        s += "   Trame : [synchro " + hexBytes(f.sync) + "]";
        if (f.counter != io::CounterField::None) s += f.counter == io::CounterField::UInt8 ? "[compteur 8 bits]" : "[compteur 16 bits]";
        s += "[" + std::to_string(f.samplesPerChannel) + " x " + std::to_string(f.channels) + " " + std::string(io::sampleTypeName(f.sample.type)) + "]";
        if (f.checksum != io::Checksum::None) s += "[" + std::string(io::checksumName(f.checksum)) + "]";
        s += ", " + std::string(f.sample.order == io::ByteOrder::LittleEndian ? "petit boutisme" : "grand boutisme") + ".\n";
    }
    s += "   Réglez l'acquisition avec le même format (préréglage enregistré dans l'application). */\n";
    return s;
}
} // namespace

std::string firmwareExample(const DecoderSettings &format, FirmwareTarget target, std::size_t channels, double sampleRateHz, int baudRate)
{
    if (format.kind == DecoderKind::FramedBinary) channels = format.frame.channels;
    else if (format.kind == DecoderKind::RawBinary) channels = format.rawChannels;
    channels = std::max<std::size_t>(1, channels);
    const double rate = sampleRateHz > 0.0 ? sampleRateHz : 100.0;
    const auto periodMicros = static_cast<long long>(std::llround(1e6 / rate));
    std::string s = header(format, channels, sampleRateHz);

    if (target == FirmwareTarget::PortableC)
    {
        s += "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n\n";
        if (format.kind == DecoderKind::FramedBinary)
        {
            s += frameBuilder(format.frame);
            s += "\n/* Utilisation, à chaque trame :\n"
                 "     static uint16_t counter = 0;\n"
                 "     uint8_t frame[QACQ_FRAME_SIZE];\n"
                 "     size_t n = qacq_build_frame(frame, counter++, samples);\n"
                 "     uart_write(frame, n);   // HAL_UART_Transmit, Serial.write, write()... */\n";
        }
        else if (format.kind == DecoderKind::RawBinary)
        {
            const std::size_t size = io::sampleSize(format.raw.type);
            s += putFunction(format.raw.order) + "\n" + floatHelpers(format.raw.type);
            s += "/* Une trame = une valeur par voie, rien d'autre : un octet perdu décale tout le flux.\n"
                 "   Préférez les trames binaires (synchro + CRC) sur une liaison série. */\n";
            s += "size_t qacq_build_raw(uint8_t *out, const " + std::string(cType(format.raw.type)) + " *values)\n{\n    size_t p = 0;\n";
            s += "    for (size_t c = 0; c < " + std::to_string(channels) + "; ++c)\n";
            s += "        p += qacq_put(out + p, " + sampleExpression(format.raw.type, "values[c]") + ", " + std::to_string(size) + ");\n";
            s += "    return p;\n}\n";
        }
        else
        {
            s += "#include <stdio.h>\n\n/* Une ligne par trame, valeurs séparées par des virgules, point décimal. */\n";
            s += "int qacq_format_line(char *out, size_t size, const float *values)\n{\n    int p = 0;\n";
            s += "    for (int c = 0; c < " + std::to_string(channels) + " && p < (int)size; ++c)\n";
            s += "        p += snprintf(out + p, size - (size_t)p, c ? \",%.4f\" : \"%.4f\", (double)values[c]);\n";
            s += "    if (p < (int)size) p += snprintf(out + p, size - (size_t)p, \"\\r\\n\");\n    return p;\n}\n";
        }
        return s;
    }

    // Arduino sketch.
    s += "\nconst unsigned long PERIOD_US = " + std::to_string(periodMicros) + "; // " + std::to_string(static_cast<long long>(rate)) +
         " échantillons/s par voie\n";
    s += "const int PINS[" + std::to_string(channels) + "] = {";
    for (std::size_t c = 0; c < channels; ++c) s += (c ? ", A" : "A") + std::to_string(c);
    s += "};\n";
    if (format.kind == DecoderKind::FramedBinary)
    {
        s += "\n" + frameBuilder(format.frame);
        s += "\n" + std::string(cType(format.frame.sample.type)) + " samples[QACQ_CHANNELS * QACQ_SAMPLES_PER_CHANNEL];\n";
        s += "uint16_t counter = 0;\nsize_t filled = 0;\nunsigned long next = 0;\n\n";
        s += "void setup()\n{\n    Serial.begin(" + std::to_string(baudRate) + ");\n    next = micros();\n}\n\n";
        s += "void loop()\n{\n    if ((long)(micros() - next) < 0) return;\n    next += PERIOD_US;\n";
        s += "    for (int c = 0; c < QACQ_CHANNELS; ++c) samples[filled++] = analogRead(PINS[c]); // 0..1023 (10 bits)\n";
        s += "    if (filled == QACQ_CHANNELS * QACQ_SAMPLES_PER_CHANNEL) {\n";
        s += "        uint8_t frame[QACQ_FRAME_SIZE];\n        size_t n = qacq_build_frame(frame, counter++, samples);\n";
        s += "        Serial.write(frame, n);\n        filled = 0;\n    }\n}\n";
        s += "\n/* Côté PC : échelle 5.0/1024 (Uno, Nano) ou 3.3/4096 (ESP32, STM32) pour lire des volts.\n";
        s += "   Débit nécessaire : " + std::to_string(static_cast<long long>(std::ceil(static_cast<double>(io::frameSize(format.frame)) * 10.0 * rate /
                                                                                          static_cast<double>(format.frame.samplesPerChannel)))) +
             " bauds au moins (10 bits par octet). */\n";
    }
    else if (format.kind == DecoderKind::RawBinary)
    {
        s += "\nunsigned long next = 0;\n\nvoid setup()\n{\n    Serial.begin(" + std::to_string(baudRate) + ");\n    next = micros();\n}\n\n";
        s += "void loop()\n{\n    if ((long)(micros() - next) < 0) return;\n    next += PERIOD_US;\n";
        s += "    for (int c = 0; c < " + std::to_string(channels) + "; ++c) {\n";
        s += "        " + std::string(cType(format.raw.type)) + " v = analogRead(PINS[c]);\n";
        s += "        Serial.write((const uint8_t *)&v, sizeof v); // AVR, ARM : petit boutisme\n    }\n}\n";
    }
    else
    {
        s += "\nunsigned long next = 0;\n\nvoid setup()\n{\n    Serial.begin(" + std::to_string(baudRate) + ");\n";
        if (format.kind == DecoderKind::Text && channels > 1)
        {
            s += "    // Ligne d'en-tête : elle donne leur nom aux voies.\n    Serial.println(\"";
            for (std::size_t c = 0; c < channels; ++c) s += (c ? ",A" : "A") + std::to_string(c);
            s += "\");\n";
        }
        s += "    next = micros();\n}\n\n";
        s += "void loop()\n{\n    if ((long)(micros() - next) < 0) return;\n    next += PERIOD_US;\n";
        s += "    for (int c = 0; c < " + std::to_string(channels) + "; ++c) {\n";
        s += "        if (c) Serial.print(',');\n        Serial.print(analogRead(PINS[c]) * (5.0 / 1024.0), 4); // volts, 4 décimales\n    }\n";
        s += "    Serial.println();\n}\n";
        s += "\n/* Texte : simple et lisible dans le moniteur série, mais ~7 octets par valeur.\n";
        s += "   Débit nécessaire : " + std::to_string(static_cast<long long>(std::ceil(7.0 * static_cast<double>(channels) * 10.0 * rate))) +
             " bauds environ. */\n";
    }
    return s;
}
} // namespace qacq::acquisition
