#include "qacq/analysis/calculus.h"

#include "qacq/analysis/measures.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace qacq::analysis
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool endsWith(std::string_view text, std::string_view end) noexcept
{
    return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
}

std::string grouped(std::size_t value)
{
    std::string digits = std::to_string(value);
    for (std::size_t i = digits.size(); i > 3; i -= 3) digits.insert(i - 3, " ");
    return digits;
}

bool hasHoles(std::span<const double> x) noexcept
{
    return std::any_of(x.begin(), x.end(), [](double v) { return !std::isfinite(v); });
}
} // namespace

double integral(std::span<const double> x, double fs) noexcept
{
    if (x.size() < 2 || !(fs > 0.0)) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i + 1 < x.size(); ++i)
    {
        if (std::isfinite(x[i]) && std::isfinite(x[i + 1])) sum += 0.5 * (x[i] + x[i + 1]);
    }
    return sum / fs;
}

std::vector<double> cumulativeIntegral(std::span<const double> x, double fs, bool removeMean)
{
    std::vector<double> y(x.size(), 0.0);
    if (x.size() < 2 || !(fs > 0.0)) return y;
    double offset = 0.0;
    if (removeMean)
    {
        double sum = 0.0;
        std::size_t count = 0;
        for (double v : x)
        {
            if (!std::isfinite(v)) continue;
            sum += v;
            ++count;
        }
        offset = count > 0 ? sum / static_cast<double>(count) : 0.0;
    }
    const double half = 0.5 / fs;
    for (std::size_t i = 0; i + 1 < x.size(); ++i)
    {
        const bool both = std::isfinite(x[i]) && std::isfinite(x[i + 1]);
        y[i + 1] = y[i] + (both ? half * ((x[i] - offset) + (x[i + 1] - offset)) : 0.0);
    }
    return y;
}

std::vector<double> derivative(std::span<const double> x, double fs)
{
    const std::size_t n = x.size();
    std::vector<double> y(n, 0.0);
    if (n < 2 || !(fs > 0.0)) return y;
    y[0] = (x[1] - x[0]) * fs;
    y[n - 1] = (x[n - 1] - x[n - 2]) * fs;
    for (std::size_t i = 1; i + 1 < n; ++i) y[i] = 0.5 * (x[i + 1] - x[i - 1]) * fs;
    return y;
}

std::vector<double> perCycle(std::span<const double> x, double fs, CycleQuantity quantity)
{
    const std::size_t n = x.size();
    std::vector<double> y(n, kNaN);
    if (n < 4 || !(fs > 0.0) || hasHoles(x)) return y;
    const std::vector<double> rises = risingCrossings(x, fs);
    for (std::size_t k = 0; k + 1 < rises.size(); ++k)
    {
        const auto a = std::min(n, static_cast<std::size_t>(std::ceil(rises[k] * fs)));
        const auto b = std::min(n, static_cast<std::size_t>(std::ceil(rises[k + 1] * fs)));
        if (b <= a) continue;
        double value = 0.0;
        if (quantity == CycleQuantity::Frequency)
        {
            const double period = rises[k + 1] - rises[k];
            value = period > 0.0 ? 1.0 / period : kNaN;
        }
        else
        {
            double sum = 0.0;
            for (std::size_t i = a; i < b; ++i) sum += quantity == CycleQuantity::Rms ? x[i] * x[i] : x[i];
            value = sum / static_cast<double>(b - a);
            if (quantity == CycleQuantity::Rms) value = std::sqrt(value);
        }
        std::fill(y.begin() + static_cast<std::ptrdiff_t>(a), y.begin() + static_cast<std::ptrdiff_t>(b), value);
    }
    return y;
}

std::string_view operationKey(Operation operation) noexcept
{
    switch (operation)
    {
    case Operation::Integral: return "integral";
    case Operation::Derivative: return "derivative";
    case Operation::CycleMean: return "cycleMean";
    case Operation::CycleRms: return "cycleRms";
    case Operation::CycleFrequency: return "cycleFrequency";
    }
    return "integral";
}

bool operationFromKey(std::string_view key, Operation &operation) noexcept
{
    for (Operation o : {Operation::Integral, Operation::Derivative, Operation::CycleMean, Operation::CycleRms,
                        Operation::CycleFrequency})
    {
        if (operationKey(o) == key)
        {
            operation = o;
            return true;
        }
    }
    return false;
}

std::string operationUnit(Operation operation, std::string_view unit)
{
    const std::string u(unit);
    switch (operation)
    {
    case Operation::Integral:
        if (u.empty()) return "s";
        if (endsWith(u, "/s")) return u.substr(0, u.size() - 2);
        return u + "·s";
    case Operation::Derivative:
        if (u.empty()) return "1/s";
        if (endsWith(u, "·s")) return u.substr(0, u.size() - std::string_view("·s").size());
        if (endsWith(u, "/s")) return u + "²";
        return u + "/s";
    case Operation::CycleMean:
    case Operation::CycleRms: return u;
    case Operation::CycleFrequency: return "Hz";
    }
    return u;
}

std::string operationChannelName(Operation operation, std::string_view channel)
{
    const std::string c(channel);
    switch (operation)
    {
    case Operation::Integral: return "∫ " + c;
    case Operation::Derivative: return "d(" + c + ")/dt";
    case Operation::CycleMean: return "Moyenne par période (" + c + ")";
    case Operation::CycleRms: return "Efficace par période (" + c + ")";
    case Operation::CycleFrequency: return "Fréquence par période (" + c + ")";
    }
    return c;
}

std::string operationTitle(Operation operation)
{
    switch (operation)
    {
    case Operation::Integral: return "Intégrale";
    case Operation::Derivative: return "Dérivée";
    case Operation::CycleMean: return "Moyenne par période";
    case Operation::CycleRms: return "Efficace par période";
    case Operation::CycleFrequency: return "Fréquence par période";
    }
    return "Calcul";
}

Derived derive(const model::Signal &source, const DeriveOptions &options)
{
    Derived out;
    if (source.representation == model::Representation::IQ)
    {
        out.error = "Les courbes calculées se font sur des voies réelles, pas sur une paire I/Q.";
        return out;
    }
    const std::size_t total = source.frameCount();
    const std::size_t first = std::min(options.firstFrame, total);
    const std::size_t count = std::min(options.frameCount, total - first);
    if (count < 2)
    {
        out.error = "La partie choisie est trop courte : il faut deux échantillons au moins.";
        return out;
    }
    if (!(source.sampleRateHz > 0.0) || !source.data)
    {
        out.error = "Ce signal n'a pas de fréquence d'échantillonnage valable.";
        return out;
    }
    std::vector<std::size_t> channels = options.channels;
    if (channels.empty())
    {
        for (std::size_t c = 0; c < source.channelCount(); ++c) channels.push_back(c);
    }
    for (std::size_t c : channels)
    {
        if (c >= source.channelCount())
        {
            out.error = "Voie " + std::to_string(c + 1) + " inexistante dans « " + source.name + " ».";
            return out;
        }
    }
    if (channels.empty())
    {
        out.error = "Aucune voie à calculer.";
        return out;
    }

    const double fs = source.sampleRateHz;
    auto data = std::make_shared<model::SampleData>();
    model::Signal &s = out.signal;
    if (options.keepSource)
    {
        for (std::size_t c : channels)
        {
            const std::span<const double> x = source.channel(c).subspan(first, count);
            data->channels.emplace_back(x.begin(), x.end());
            s.channels.push_back(source.channels[c]);
        }
    }
    std::vector<std::string> remarks;
    std::size_t usable = 0;
    for (std::size_t c : channels)
    {
        const std::span<const double> x = source.channel(c).subspan(first, count);
        std::vector<double> y;
        switch (options.operation)
        {
        case Operation::Integral: y = cumulativeIntegral(x, fs, options.removeMean); break;
        case Operation::Derivative: y = derivative(x, fs); break;
        case Operation::CycleMean: y = perCycle(x, fs, CycleQuantity::Mean); break;
        case Operation::CycleRms: y = perCycle(x, fs, CycleQuantity::Rms); break;
        case Operation::CycleFrequency: y = perCycle(x, fs, CycleQuantity::Frequency); break;
        }
        const bool perPeriod = options.operation == Operation::CycleMean || options.operation == Operation::CycleRms ||
                               options.operation == Operation::CycleFrequency;
        if (perPeriod && std::none_of(y.begin(), y.end(), [](double v) { return std::isfinite(v); }))
        {
            remarks.push_back("« " + source.channels[c].name + " » : " +
                              (hasHoles(x) ? std::string("des échantillons manquent, pas de valeur par période.")
                                           : std::string("moins de deux fronts montants, pas de période entière.")));
        }
        else ++usable;
        data->channels.push_back(std::move(y));
        s.channels.push_back({operationChannelName(options.operation, source.channels[c].name),
                              operationUnit(options.operation, source.channels[c].unit)});
    }
    if (usable == 0)
    {
        out.signal = {};
        out.error = remarks.size() == 1 ? remarks.front() : "Aucune voie ne se prête à ce calcul : " + remarks.front();
        return out;
    }
    for (std::size_t d : source.data->discontinuities)
    {
        if (d > first && d < first + count) data->discontinuities.push_back(d - first);
    }

    s.name = operationTitle(options.operation) + " de « " + source.name + " »";
    s.origin = model::Origin::Derived;
    s.representation = model::Representation::Real;
    s.sampleRateHz = fs;
    s.startSeconds = source.startSeconds + static_cast<double>(first) / fs;
    s.data = std::move(data);

    std::string notes;
    const bool whole = first == 0 && count == total;
    const std::string part = whole ? "tout le signal" : grouped(count) + " échantillons à partir du n° " + grouped(first);
    switch (options.operation)
    {
    case Operation::Integral:
        notes = "Intégrale (méthode des trapèzes) de « " + source.name + " », sur " + part + ", partant de 0" +
                (options.removeMean ? ", moyenne retirée d'abord." : ".");
        break;
    case Operation::Derivative:
        notes = "Dérivée (différences centrées) de « " + source.name + " », sur " + part + ".";
        break;
    default:
        notes = operationTitle(options.operation) + " de « " + source.name + " », sur " + part +
                " : une valeur par période (d'un front montant au suivant), vide avant la première et après la dernière "
                "période entière.";
        break;
    }
    for (const std::string &r : remarks) notes += "\n" + r;
    s.notes = notes;

    model::Json parameters = model::Json::object();
    parameters["firstFrame"] = static_cast<unsigned long long>(first);
    parameters["frameCount"] = static_cast<unsigned long long>(count);
    model::Json list = model::Json::array();
    for (std::size_t c : channels) list.push_back(static_cast<unsigned long long>(c));
    parameters["channels"] = list;
    parameters["keepSource"] = options.keepSource;
    if (options.operation == Operation::Integral) parameters["removeMean"] = options.removeMean;
    s.provenance = model::Json::object();
    s.provenance["operation"] = operationKey(options.operation);
    s.provenance["source"] = source.name;
    s.provenance["sourceId"] = static_cast<unsigned long long>(source.id);
    s.provenance["parameters"] = parameters;
    return out;
}
} // namespace qacq::analysis
