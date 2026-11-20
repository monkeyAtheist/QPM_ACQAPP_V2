#include "qacq/dsp/channel.h"

#include "qacq/dsp/random.h"

#include <algorithm>
#include <cmath>

namespace qacq::dsp
{
namespace
{
bool prepare(const AwgnSpec &spec, double fs, double power, bool complexRecord, AwgnResult &result)
{
    result.signalPower = power;
    if (!(std::isfinite(fs) && fs > 0.0))
    {
        result.error = "La fréquence d'échantillonnage doit être positive.";
        return false;
    }
    if (!std::isfinite(spec.valueDb))
    {
        result.error = "Le rapport signal sur bruit doit être fini.";
        return false;
    }
    if (!(power > 0.0))
    {
        result.error = "Le signal est nul : impossible de régler le bruit par rapport à lui.";
        return false;
    }
    const double ratio = std::pow(10.0, spec.valueDb / 10.0);
    if (spec.level == AwgnSpec::Level::EbN0Db)
    {
        if (!(spec.bitRate > 0.0))
        {
            result.error = "Eb/N0 demande un débit binaire positif.";
            return false;
        }
        result.n0 = power / spec.bitRate / ratio;
        result.noiseVariance = 0.5 * result.n0 * fs; // per real sample, or per component
    }
    else
    {
        const double noisePower = power / ratio;
        result.noiseVariance = complexRecord ? 0.5 * noisePower : noisePower;
        result.n0 = complexRecord ? noisePower / fs : 2.0 * noisePower / fs;
    }
    return true;
}

void measured(AwgnResult &result, double drawnPower)
{
    if (drawnPower > 0.0) result.measuredSnrDb = 10.0 * std::log10(result.signalPower / drawnPower);
}
} // namespace

AwgnResult addAwgn(std::vector<double> &signal, double fs, const AwgnSpec &spec)
{
    AwgnResult result;
    double power = 0.0;
    for (double v : signal) power += v * v;
    power /= static_cast<double>(std::max<std::size_t>(1, signal.size()));
    if (!prepare(spec, fs, power, false, result)) return result;
    Random random(spec.seed);
    const double sigma = std::sqrt(result.noiseVariance);
    double drawn = 0.0;
    for (double &v : signal)
    {
        const double w = sigma * random.normal();
        v += w;
        drawn += w * w;
    }
    drawn /= static_cast<double>(signal.size());
    measured(result, drawn);
    if (spec.bitRate > 0.0)
        result.measuredEbN0Db = 10.0 * std::log10((power / spec.bitRate) / (2.0 * drawn / fs));
    return result;
}

AwgnResult addAwgn(std::vector<double> &inPhase, std::vector<double> &quadrature, double fs, const AwgnSpec &spec)
{
    AwgnResult result;
    const std::size_t n = std::min(inPhase.size(), quadrature.size());
    double power = 0.0;
    for (std::size_t i = 0; i < n; ++i) power += inPhase[i] * inPhase[i] + quadrature[i] * quadrature[i];
    power /= static_cast<double>(std::max<std::size_t>(1, n));
    if (!prepare(spec, fs, power, true, result)) return result;
    Random random(spec.seed);
    const double sigma = std::sqrt(result.noiseVariance);
    double drawn = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double wi = sigma * random.normal();
        const double wq = sigma * random.normal();
        inPhase[i] += wi;
        quadrature[i] += wq;
        drawn += wi * wi + wq * wq;
    }
    drawn /= static_cast<double>(n);
    measured(result, drawn);
    if (spec.bitRate > 0.0) result.measuredEbN0Db = 10.0 * std::log10((power / spec.bitRate) / (drawn / fs));
    return result;
}
} // namespace qacq::dsp
