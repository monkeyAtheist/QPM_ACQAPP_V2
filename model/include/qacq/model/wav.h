#pragma once

// WAV files (sound card recordings, audio test signals, I/Q recordings of software radios).
//
// Read: PCM 8 bits (unsigned), 16, 24 and 32 bits, IEEE float 32 and 64 bits, plain or
// WAVE_FORMAT_EXTENSIBLE, any number of channels; unknown chunks are skipped. Integer samples are scaled
// to full scale 1 (-32768 -> -1, 32767 -> 0.99997).
// Write: PCM 16, 24, 32 bits or float 32 bits. The value fullScale maps to the largest code (with
// normalize, the peak of the signal does); values beyond are clipped and counted in a warning.

#include "qacq/model/csv.h" // ImportResult
#include "qacq/model/signal.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace qacq::model
{
enum class WavEncoding
{
    Pcm16,
    Pcm24,
    Pcm32,
    Float32
};

struct WavWriteOptions
{
    WavEncoding encoding = WavEncoding::Pcm16;
    double fullScale = 1.0;
    bool normalize = false; // full scale = peak of the signal
};

std::string writeWav(std::ostream &out, const Signal &signal, const WavWriteOptions &options = {},
                     std::vector<std::string> *warnings = nullptr);
std::string exportWav(const std::filesystem::path &file, const Signal &signal, const WavWriteOptions &options = {},
                      std::vector<std::string> *warnings = nullptr);

ImportResult readWav(std::istream &in, const std::string &name);
ImportResult importWav(const std::filesystem::path &file);
} // namespace qacq::model
