#pragma once

// Additive white Gaussian noise, set by a signal-to-noise ratio or by Eb/N0.
//
// The signal power P is measured on the record. With Eb/N0: Eb = P / Rb and N0 = Eb / (Eb/N0); a real
// sampled record gets noise of variance N0 * fs / 2 (two-sided density N0/2 over -fs/2..fs/2), a complex
// record N0 * fs / 2 on each of I and Q (N0 * fs in total). With an SNR the noise power is P / SNR over
// the whole sampled band (0..fs/2 for a real record, -fs/2..fs/2 for a complex one).

#include <cstdint>
#include <string>
#include <vector>

namespace qacq::dsp
{
struct AwgnSpec
{
    enum class Level
    {
        SnrDb,
        EbN0Db
    };
    Level level = Level::EbN0Db;
    double valueDb = 10.0;
    double bitRate = 0.0; // needed for Eb/N0
    std::uint64_t seed = 1;
};

struct AwgnResult
{
    double signalPower = 0.0;    // mean square of the record (I^2 + Q^2 for a complex record)
    double noiseVariance = 0.0;  // per real sample, or per component for a complex record
    double n0 = 0.0;             // one-sided noise density, unit^2/Hz
    double measuredSnrDb = 0.0;  // from the noise actually drawn
    double measuredEbN0Db = 0.0; // idem, when a bit rate is known
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

AwgnResult addAwgn(std::vector<double> &signal, double sampleRateHz, const AwgnSpec &spec);
AwgnResult addAwgn(std::vector<double> &inPhase, std::vector<double> &quadrature, double sampleRateHz,
                   const AwgnSpec &spec);
} // namespace qacq::dsp
