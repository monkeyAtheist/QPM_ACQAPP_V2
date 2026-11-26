#include "qacq/model/signal.h"

#include "qacq/io/decoder.h"

#include <algorithm>
#include <cmath>

namespace qacq::model
{
std::string_view originKey(Origin origin) noexcept
{
    switch (origin)
    {
        case Origin::Acquired: return "acquired";
        case Origin::Generated: return "generated";
        case Origin::Imported: return "imported";
        case Origin::Derived: return "derived";
    }
    return "generated";
}

std::string_view originName(Origin origin) noexcept
{
    switch (origin)
    {
        case Origin::Acquired: return "Acquis";
        case Origin::Generated: return "Généré";
        case Origin::Imported: return "Importé";
        case Origin::Derived: return "Calculé";
    }
    return "?";
}

bool originFromKey(std::string_view key, Origin &origin) noexcept
{
    for (Origin o : {Origin::Acquired, Origin::Generated, Origin::Imported, Origin::Derived})
    {
        if (originKey(o) == key)
        {
            origin = o;
            return true;
        }
    }
    return false;
}

std::span<const double> Signal::channel(std::size_t index) const noexcept
{
    if (!data || index >= data->channels.size()) return {};
    return data->channels[index];
}

std::string validateSignal(const Signal &signal)
{
    if (!(std::isfinite(signal.sampleRateHz) && signal.sampleRateHz > 0.0))
        return "La fréquence d'échantillonnage doit être positive et finie.";
    if (!std::isfinite(signal.startSeconds)) return "L'instant de départ doit être fini.";
    if (signal.channels.empty()) return "Un signal a au moins une voie.";
    if (!signal.data) return "Le signal n'a pas de données.";
    if (signal.data->channelCount() != signal.channels.size())
        return "Le nombre de voies décrites ne correspond pas aux données.";
    const std::size_t frames = signal.data->frameCount();
    for (const auto &c : signal.data->channels)
        if (c.size() != frames) return "Toutes les voies doivent avoir le même nombre d'échantillons.";
    std::size_t previous = 0;
    for (std::size_t d : signal.data->discontinuities)
    {
        if (d > frames || d < previous) return "Les ruptures doivent être croissantes et dans l'enregistrement.";
        previous = d;
    }
    if (signal.representation == Representation::IQ && signal.channels.size() % 2 != 0)
        return "Un signal I/Q a un nombre pair de voies (I, Q).";
    return {};
}

bool keepChannels(Signal &signal, std::span<const std::size_t> channels, std::string *error)
{
    const auto fail = [error](const char *why) {
        if (error) *error = why;
        return false;
    };
    const std::size_t count = signal.channels.size();
    if (!signal.data || signal.data->channelCount() != count) return fail("Le signal n'a pas de données.");
    if (channels.empty()) return fail("Un signal garde au moins une voie.");
    std::vector<bool> kept(count, false);
    for (const std::size_t c : channels)
    {
        if (c >= count) return fail("Cette voie n'existe pas.");
        if (kept[c]) return fail("Une voie ne peut être gardée qu'une fois.");
        kept[c] = true;
    }

    auto data = std::make_shared<SampleData>();
    data->discontinuities = signal.data->discontinuities;
    data->channels.reserve(channels.size());
    std::vector<ChannelInfo> infos;
    infos.reserve(channels.size());
    for (const std::size_t c : channels)
    {
        data->channels.push_back(signal.data->channels[c]);
        infos.push_back(signal.channels[c]);
    }
    // I/Q: whole pairs, I then Q, as they were.
    if (signal.representation == Representation::IQ)
    {
        bool pairs = channels.size() % 2 == 0;
        for (std::size_t k = 0; pairs && k + 1 < channels.size(); k += 2)
            pairs = channels[k] % 2 == 0 && channels[k + 1] == channels[k] + 1;
        if (!pairs) signal.representation = Representation::Real;
    }
    std::string gone;
    std::size_t removed = 0;
    for (std::size_t c = 0; c < count; ++c)
    {
        if (kept[c]) continue;
        gone += (removed > 0 ? ", " : "") + signal.channels[c].name;
        ++removed;
    }
    if (removed > 0)
    {
        if (!signal.notes.empty() && signal.notes.back() != '\n') signal.notes += '\n';
        signal.notes += (removed > 1 ? "Voies retirées : " : "Voie retirée : ") + gone + ".";
    }
    signal.channels = std::move(infos);
    signal.data = std::move(data);
    return true;
}

std::shared_ptr<SampleData> deinterleave(std::span<const double> interleaved, std::size_t channels)
{
    auto data = std::make_shared<SampleData>();
    if (channels == 0) return data;
    const std::size_t frames = interleaved.size() / channels;
    data->channels.assign(channels, std::vector<double>(frames));
    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t c = 0; c < channels; ++c) data->channels[c][f] = interleaved[f * channels + c];
    return data;
}

std::shared_ptr<SampleData> deinterleave(const io::SampleBlock &block)
{
    auto data = deinterleave(block.values, block.channelCount);
    data->discontinuities = block.discontinuities;
    return data;
}

std::vector<ChannelInfo> defaultChannels(std::size_t count, std::string_view unit, Representation representation)
{
    std::vector<ChannelInfo> channels(count);
    for (std::size_t c = 0; c < count; ++c)
    {
        if (representation == Representation::IQ)
        {
            const std::string pair = count > 2 ? " " + std::to_string(c / 2 + 1) : std::string();
            channels[c].name = (c % 2 == 0 ? "I" : "Q") + pair;
        }
        else
        {
            channels[c].name = "Voie " + std::to_string(c + 1);
        }
        channels[c].unit = std::string(unit);
    }
    return channels;
}
} // namespace qacq::model
