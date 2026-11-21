#include "qacq/analysis/curves.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace qacq::analysis
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr CurveKind kKinds[] = {CurveKind::Spectrum,  CurveKind::Source,   CurveKind::Integral,
                                CurveKind::Derivative, CurveKind::CycleMean, CurveKind::CycleRms,
                                CurveKind::CycleFrequency};

// The operation of calculus.h behind a computed time curve.
bool operationOf(CurveKind kind, Operation &operation) noexcept
{
    switch (kind)
    {
    case CurveKind::Integral: operation = Operation::Integral; return true;
    case CurveKind::Derivative: operation = Operation::Derivative; return true;
    case CurveKind::CycleMean: operation = Operation::CycleMean; return true;
    case CurveKind::CycleRms: operation = Operation::CycleRms; return true;
    case CurveKind::CycleFrequency: operation = Operation::CycleFrequency; return true;
    case CurveKind::Spectrum:
    case CurveKind::Source: return false;
    }
    return false;
}

std::size_t toSize(double value, std::size_t fallback) noexcept
{
    if (!std::isfinite(value) || value < 0.0) return fallback;
    if (value >= 9.0e15) return fallback;
    return static_cast<std::size_t>(value);
}
} // namespace

std::string_view curveKindKey(CurveKind kind) noexcept
{
    switch (kind)
    {
    case CurveKind::Spectrum: return "spectrum";
    case CurveKind::Source: return "source";
    case CurveKind::Integral: return "integral";
    case CurveKind::Derivative: return "derivative";
    case CurveKind::CycleMean: return "cycleMean";
    case CurveKind::CycleRms: return "cycleRms";
    case CurveKind::CycleFrequency: return "cycleFrequency";
    }
    return "spectrum";
}

bool curveKindFromKey(std::string_view key, CurveKind &kind) noexcept
{
    for (CurveKind k : kKinds)
    {
        if (curveKindKey(k) == key)
        {
            kind = k;
            return true;
        }
    }
    return false;
}

bool isTimeCurve(CurveKind kind) noexcept
{
    return kind != CurveKind::Spectrum;
}

std::string curveKindName(CurveKind kind)
{
    switch (kind)
    {
    case CurveKind::Spectrum: return "Spectre";
    case CurveKind::Source: return "Voie d'origine";
    case CurveKind::Integral: return "Intégrale";
    case CurveKind::Derivative: return "Dérivée";
    case CurveKind::CycleMean: return "Moyenne par période";
    case CurveKind::CycleRms: return "Efficace par période";
    case CurveKind::CycleFrequency: return "Fréquence par période";
    }
    return "Courbe";
}

bool CurveSpec::samePlace(const CurveSpec &other) const noexcept
{
    return kind == other.kind && channel == other.channel && firstFrame == other.firstFrame &&
           frameCount == other.frameCount && (kind != CurveKind::Integral || removeMean == other.removeMean);
}

model::Json toJson(const std::vector<CurveSpec> &curves)
{
    model::Json list = model::Json::array();
    for (const CurveSpec &c : curves)
    {
        model::Json j = model::Json::object();
        j["id"] = static_cast<unsigned long long>(c.id);
        j["kind"] = curveKindKey(c.kind);
        j["channel"] = static_cast<unsigned long long>(c.channel);
        j["visible"] = c.visible;
        if (c.kind == CurveKind::Integral) j["removeMean"] = c.removeMean;
        if (isTimeCurve(c.kind))
        {
            j["firstFrame"] = static_cast<unsigned long long>(c.firstFrame);
            if (c.frameCount != std::numeric_limits<std::size_t>::max())
                j["frameCount"] = static_cast<unsigned long long>(c.frameCount);
        }
        list.push_back(std::move(j));
    }
    return list;
}

std::vector<CurveSpec> curveSpecsFromJson(const model::Json &json)
{
    std::vector<CurveSpec> curves;
    if (!json.isArray()) return curves;
    std::set<std::uint64_t> ids;
    std::uint64_t next = 1;
    for (const model::Json &j : json.items())
    {
        if (!j.isObject()) continue;
        CurveSpec c;
        if (!curveKindFromKey(j["kind"].toString(), c.kind)) continue;
        const double channel = j["channel"].toNumber(-1.0);
        if (!(channel >= 0.0) || channel > 4096.0) continue;
        c.channel = static_cast<std::size_t>(channel);
        c.visible = j["visible"].toBool(true);
        c.removeMean = j["removeMean"].toBool(false);
        c.firstFrame = toSize(j["firstFrame"].toNumber(0.0), 0);
        c.frameCount = j.contains("frameCount") ? toSize(j["frameCount"].toNumber(-1.0), std::numeric_limits<std::size_t>::max())
                                                 : std::numeric_limits<std::size_t>::max();
        c.id = static_cast<std::uint64_t>(toSize(j["id"].toNumber(0.0), 0));
        curves.push_back(c);
        if (c.id >= next) next = c.id + 1;
    }
    // Missing or repeated ids get new ones.
    for (CurveSpec &c : curves)
    {
        if (c.id == 0 || !ids.insert(c.id).second)
        {
            c.id = next++;
            ids.insert(c.id);
        }
    }
    return curves;
}

std::string curveName(const CurveSpec &curve, const model::Signal &signal)
{
    if (curve.channel >= signal.channelCount()) return {};
    const std::string &channel = signal.channels[curve.channel].name;
    Operation operation = Operation::Integral;
    if (operationOf(curve.kind, operation)) return operationChannelName(operation, channel);
    return channel;
}

std::string curveUnit(const CurveSpec &curve, const model::Signal &signal)
{
    if (curve.channel >= signal.channelCount()) return {};
    const std::string &unit = signal.channels[curve.channel].unit;
    Operation operation = Operation::Integral;
    if (operationOf(curve.kind, operation)) return operationUnit(operation, unit);
    return unit;
}

std::pair<std::size_t, std::size_t> curvePart(const CurveSpec &curve, const model::Signal &signal)
{
    const std::size_t frames = signal.frameCount();
    const std::size_t first = std::min(curve.firstFrame, frames);
    return {first, std::min(curve.frameCount, frames - first)};
}

std::vector<double> computeTimeCurve(const model::Signal &signal, const CurveSpec &curve)
{
    Operation operation = Operation::Integral;
    if (!operationOf(curve.kind, operation) || curve.channel >= signal.channelCount() || !signal.data ||
        signal.channelCount() != signal.data->channelCount())
        return {};
    const auto [first, count] = curvePart(curve, signal);
    const std::span<const double> x = signal.channel(curve.channel).subspan(first, count);
    const double fs = signal.sampleRateHz;
    switch (operation)
    {
    case Operation::Integral: return cumulativeIntegral(x, fs, curve.removeMean);
    case Operation::Derivative: return derivative(x, fs);
    case Operation::CycleMean: return perCycle(x, fs, CycleQuantity::Mean);
    case Operation::CycleRms: return perCycle(x, fs, CycleQuantity::Rms);
    case Operation::CycleFrequency: return perCycle(x, fs, CycleQuantity::Frequency);
    }
    return {};
}

Derived signalFromCurves(const TimeCurveSet &set)
{
    Derived out;
    if (!set.signal || !(set.signal->sampleRateHz > 0.0))
    {
        out.error = "Pas de signal d'origine pour ces courbes.";
        return out;
    }
    std::vector<const TimeCurve *> kept;
    std::size_t lo = std::numeric_limits<std::size_t>::max(), hi = 0;
    for (const TimeCurve &c : set.curves)
    {
        if (!c.visible || c.values.empty()) continue;
        kept.push_back(&c);
        lo = std::min(lo, c.firstFrame);
        hi = std::max(hi, c.firstFrame + c.values.size());
    }
    if (kept.empty() || hi <= lo)
    {
        out.error = "Aucune courbe affichée à garder.";
        return out;
    }
    const model::Signal &source = *set.signal;
    const std::size_t n = hi - lo;
    auto data = std::make_shared<model::SampleData>();
    model::Signal &s = out.signal;
    std::string list;
    for (const TimeCurve *c : kept)
    {
        std::vector<double> values(n, kNaN);
        std::copy(c->values.begin(), c->values.end(), values.begin() + static_cast<std::ptrdiff_t>(c->firstFrame - lo));
        data->channels.push_back(std::move(values));
        s.channels.push_back({c->name, c->unit});
        list += (list.empty() ? "" : ", ") + c->name;
    }
    if (source.data)
    {
        for (std::size_t d : source.data->discontinuities)
        {
            if (d > lo && d < hi) data->discontinuities.push_back(d - lo);
        }
    }
    s.name = "Courbes de « " + source.name + " »";
    s.origin = model::Origin::Derived;
    s.representation = model::Representation::Real;
    s.sampleRateHz = source.sampleRateHz;
    s.startSeconds = source.startSeconds + static_cast<double>(lo) / source.sampleRateHz;
    s.data = std::move(data);
    s.notes = "Courbes du module Analyse, calculées à partir de « " + source.name + " » : " + list +
              ". Vide (échantillons manquants) là où une courbe n'a pas de valeur.";
    s.provenance = model::Json::object();
    s.provenance["operation"] = "curves";
    s.provenance["source"] = source.name;
    s.provenance["sourceId"] = static_cast<unsigned long long>(source.id);
    model::Json names = model::Json::array();
    for (const TimeCurve *c : kept) names.push_back(c->name);
    s.provenance["curves"] = names;
    return out;
}
} // namespace qacq::analysis
