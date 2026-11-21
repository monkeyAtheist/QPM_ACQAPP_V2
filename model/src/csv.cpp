#include "qacq/model/csv.h"

#include "file_names.h"
#include "qacq/io/text_decoder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <istream>
#include <ostream>

namespace qacq::model
{
namespace
{
std::string csvNumber(double v, bool decimalComma)
{
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    std::string s = formatNumber(v);
    if (decimalComma) std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string csvText(const std::string &text, char separator)
{
    // Quote a title that holds the separator or a quote (RFC 4180).
    if (text.find(separator) == std::string::npos && text.find('"') == std::string::npos) return text;
    std::string quoted = "\"";
    for (char c : text)
    {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    return quoted + "\"";
}

std::string trim(std::string s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// Splits a header line on the column separators (not on spaces: titles hold spaces). Quotes are honoured.
std::vector<std::string> splitHeader(const std::string &line, bool decimalComma)
{
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (quoted)
        {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') field += '"', ++i;
            else if (c == '"') quoted = false;
            else field += c;
        }
        else if (c == '"')
        {
            quoted = true;
        }
        else if (c == ';' || c == '\t' || (!decimalComma && c == ','))
        {
            fields.push_back(trim(field));
            field.clear();
        }
        else
        {
            field += c;
        }
    }
    fields.push_back(trim(field));
    return fields;
}
} // namespace

ChannelInfo parseColumnTitle(const std::string &title)
{
    ChannelInfo info;
    std::string t = trim(title);
    if (t.size() >= 3 && (t.back() == ')' || t.back() == ']'))
    {
        const char open = t.back() == ')' ? '(' : '[';
        const std::size_t at = t.rfind(open);
        if (at != std::string::npos && at > 0)
        {
            info.unit = trim(t.substr(at + 1, t.size() - at - 2));
            t = trim(t.substr(0, at));
        }
    }
    info.name = t;
    return info;
}

std::string writeCsv(std::ostream &out, const Signal &signal, const CsvExportOptions &options)
{
    const std::string problem = validateSignal(signal);
    if (!problem.empty()) return problem;
    const char sep = options.decimalComma ? ';' : ',';
    if (options.header)
    {
        std::string line;
        if (options.timeColumn) line += "temps (s)";
        for (std::size_t c = 0; c < signal.channels.size(); ++c)
        {
            if (!line.empty() || c > 0) line += sep;
            const ChannelInfo &info = signal.channels[c];
            line += csvText(info.unit.empty() ? info.name : info.name + " (" + info.unit + ")", sep);
        }
        out << line << "\r\n";
    }
    const std::size_t frames = signal.frameCount();
    std::string line;
    for (std::size_t f = 0; f < frames; ++f)
    {
        line.clear();
        if (options.timeColumn)
            line += csvNumber(signal.startSeconds + static_cast<double>(f) / signal.sampleRateHz, options.decimalComma);
        for (std::size_t c = 0; c < signal.channels.size(); ++c)
        {
            if (options.timeColumn || c > 0) line += sep;
            line += csvNumber(signal.data->channels[c][f], options.decimalComma);
        }
        line += "\r\n";
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    out.flush();
    if (!out) return "Écriture impossible (disque plein ou fichier protégé ?).";
    return {};
}

std::string exportCsv(const std::filesystem::path &file, const Signal &signal, const CsvExportOptions &options)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return "Impossible de créer « " + detail::displayName(file) + " ».";
    return writeCsv(out, signal, options);
}

ImportResult readCsv(std::istream &in, const std::string &name, const CsvImportOptions &options)
{
    ImportResult result;
    Signal &signal = result.signal;
    signal.name = name.empty() ? "Import CSV" : name;
    signal.origin = Origin::Imported;

    // Header: the first line that is neither empty nor a comment, if none of its fields is a number.
    std::vector<std::string> titles;
    std::string first;
    bool firstIsData = false;
    while (std::getline(in, first))
    {
        if (first.size() >= 3 && static_cast<unsigned char>(first[0]) == 0xEF &&
            static_cast<unsigned char>(first[1]) == 0xBB && static_cast<unsigned char>(first[2]) == 0xBF)
            first.erase(0, 3); // UTF-8 byte order mark (Excel)
        const std::string t = trim(first);
        if (t.empty() || t.front() == '#') continue;
        const auto fields = splitHeader(t, options.decimalComma);
        const auto whole = io::parseNumberLine(t, options.decimalComma);
        const bool anyNumber = (whole && !whole->empty()) ||
                               std::any_of(fields.begin(), fields.end(), [&](const std::string &f) {
                                   const auto v = io::parseNumberLine(f, options.decimalComma);
                                   return v && !v->empty();
                               });
        if (anyNumber) firstIsData = true;
        else titles = fields;
        break;
    }

    io::TextFormat format;
    format.decimalComma = options.decimalComma;
    format.maximumLineLength = 1 << 20;
    format.channelCount = titles.size();
    io::TextDecoder decoder(format);
    io::SampleBlock block;
    if (firstIsData)
    {
        first += '\n';
        decoder.decode({reinterpret_cast<const std::uint8_t *>(first.data()), first.size()}, block);
    }
    std::vector<char> buffer(1 << 20);
    while (in)
    {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        decoder.decode({reinterpret_cast<const std::uint8_t *>(buffer.data()), got}, block);
    }
    decoder.decode({reinterpret_cast<const std::uint8_t *>("\n"), 1}, block); // a last line without newline

    const auto &stats = decoder.statistics();
    if (stats.rejected > 0)
        result.warnings.push_back(std::to_string(stats.rejected) + " ligne(s) illisible(s) ou de longueur différente ignorée(s).");
    if (block.frameCount() == 0)
    {
        result.error = "Aucune ligne de nombres dans ce fichier.";
        return result;
    }
    std::size_t columns = block.channelCount;
    auto data = deinterleave(block);
    std::vector<ChannelInfo> infos;
    for (std::size_t c = 0; c < columns; ++c)
    {
        ChannelInfo info = c < titles.size() && !titles[c].empty() ? parseColumnTitle(titles[c])
                                                                    : ChannelInfo{"Voie " + std::to_string(c + 1), {}};
        if (info.unit.empty()) info.unit = options.unit;
        infos.push_back(std::move(info));
    }

    signal.sampleRateHz = options.sampleRateHz;
    if (options.firstColumnIsTime)
    {
        if (columns < 2)
        {
            result.error = "Il faut au moins une colonne de valeurs après la colonne de temps.";
            return result;
        }
        std::vector<double> time = std::move(data->channels.front());
        if (options.timeUnitSeconds != 1.0 && options.timeUnitSeconds > 0.0)
        {
            for (double &t : time) t *= options.timeUnitSeconds;
        }
        data->channels.erase(data->channels.begin());
        infos.erase(infos.begin());
        if (time.size() < 2)
        {
            result.error = "Une seule ligne : la colonne de temps ne donne pas de fréquence d'échantillonnage.";
            return result;
        }
        std::vector<double> steps(time.size() - 1);
        for (std::size_t i = 1; i < time.size(); ++i) steps[i - 1] = time[i] - time[i - 1];
        std::vector<double> sorted = steps;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        const double step = sorted[sorted.size() / 2];
        if (!(step > 0.0) || !std::isfinite(step))
        {
            result.error = "La colonne de temps n'est pas croissante.";
            return result;
        }
        double rate = 1.0 / step;
        if (std::abs(rate - std::round(rate)) < 1e-6 * rate) rate = std::round(rate);
        signal.sampleRateHz = rate;
        signal.startSeconds = time.front();
        double worst = 0.0;
        bool decreasing = false;
        for (double s : steps)
        {
            worst = std::max(worst, std::abs(s - step) / step);
            decreasing = decreasing || !(s > 0.0);
        }
        if (decreasing)
            result.warnings.emplace_back("La colonne de temps recule ou se répète par endroits.");
        if (worst > 1e-3)
            result.warnings.push_back("Pas de temps irrégulier (écart jusqu'à " + formatNumber(std::round(worst * 1000.0) / 10.0) +
                                      " %) : les échantillons sont supposés régulièrement espacés.");
    }
    else if (!(options.sampleRateHz > 0.0))
    {
        result.error = "La fréquence d'échantillonnage doit être positive.";
        return result;
    }
    signal.channels = std::move(infos);
    signal.data = std::move(data);
    signal.provenance = Json::object();
    signal.provenance["import"] = "csv";
    const std::string problem = validateSignal(signal);
    if (!problem.empty()) result.error = problem;
    return result;
}

ImportResult importCsv(const std::filesystem::path &file, const CsvImportOptions &options)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        ImportResult r;
        r.error = "Impossible d'ouvrir « " + detail::displayName(file) + " ».";
        return r;
    }
    const std::u8string stem = file.stem().u8string();
    ImportResult r = readCsv(in, std::string(reinterpret_cast<const char *>(stem.data()), stem.size()), options);
    r.signal.provenance["file"] = detail::displayName(file);
    return r;
}

namespace
{
bool digit(char c)
{
    return c >= '0' && c <= '9';
}

// A field "1,5", "-2,25e-3" or "12": a number written with a decimal comma (or an integer).
bool commaNumber(const std::string &field)
{
    std::size_t i = 0, commas = 0, digits = 0;
    if (i < field.size() && (field[i] == '-' || field[i] == '+')) ++i;
    for (; i < field.size(); ++i)
    {
        const char c = field[i];
        if (digit(c)) ++digits;
        else if (c == ',') ++commas;
        else if ((c == 'e' || c == 'E') && digits > 0) break;
        else return false;
    }
    if (i < field.size())
    {
        ++i;
        if (i < field.size() && (field[i] == '-' || field[i] == '+')) ++i;
        if (i == field.size()) return false;
        for (; i < field.size(); ++i)
        {
            if (!digit(field[i])) return false;
        }
    }
    return digits > 0 && commas <= 1;
}

std::string lowerAscii(std::string s)
{
    for (char &c : s)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}
} // namespace

CsvSniff sniffCsv(std::istream &in, std::size_t maximumLines)
{
    CsvSniff sniff;
    std::vector<std::string> data;
    std::string headerLine;
    std::string line;
    for (std::size_t read = 0; read < maximumLines && std::getline(in, line); ++read)
    {
        if (read == 0 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (sniff.preview.size() < 8) sniff.preview.push_back(line);
        const std::string t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        const auto asPoint = io::parseNumberLine(t, false);
        const auto asComma = io::parseNumberLine(t, true);
        const bool numbers = (asPoint && !asPoint->empty()) || (asComma && !asComma->empty());
        if (!numbers && data.empty() && headerLine.empty())
        {
            headerLine = t;
            continue;
        }
        if (numbers) data.push_back(t);
    }

    // Decimal mark.
    bool separators = false, point = false, comma = false;
    for (const std::string &d : data)
    {
        if (d.find(';') != std::string::npos || d.find('\t') != std::string::npos) separators = true;
        for (std::size_t i = 1; i + 1 < d.size(); ++i)
        {
            if (!digit(d[i - 1]) || !digit(d[i + 1])) continue;
            if (d[i] == '.') point = true;
            if (d[i] == ',') comma = true;
        }
    }
    if (comma && !point)
    {
        if (separators)
        {
            sniff.decimalComma = true;
        }
        else
        {
            // "1,5 2,5": spaces between the columns, one comma in each number. "512,1023" alone stays two
            // integers (an Arduino printing two values), the usual case.
            bool all = true, spaced = false;
            for (const std::string &d : data)
            {
                std::size_t start = 0, fields = 0;
                while (start < d.size() && all)
                {
                    while (start < d.size() && d[start] == ' ') ++start;
                    std::size_t end = start;
                    while (end < d.size() && d[end] != ' ') ++end;
                    if (end > start)
                    {
                        all = commaNumber(d.substr(start, end - start));
                        ++fields;
                    }
                    start = end;
                }
                spaced = spaced || fields >= 2;
            }
            sniff.decimalComma = all && spaced;
        }
    }

    // Columns, header.
    std::vector<std::vector<double>> rows;
    for (const std::string &d : data)
    {
        if (auto v = io::parseNumberLine(d, sniff.decimalComma); v && !v->empty()) rows.push_back(std::move(*v));
    }
    sniff.lines = rows.size();
    if (!rows.empty()) sniff.columns = rows.front().size();
    if (!headerLine.empty())
    {
        sniff.header = true;
        sniff.titles = splitHeader(headerLine, sniff.decimalComma);
    }

    // Time column: named so, or (without header) growing by a regular step.
    if (sniff.columns < 2 || rows.size() < 2) return sniff;
    std::vector<double> steps;
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
        if (rows[i].size() == sniff.columns && rows[i - 1].size() == sniff.columns) steps.push_back(rows[i][0] - rows[i - 1][0]);
    }
    if (steps.empty()) return sniff;
    // The order of the steps does not matter below: the median is taken in place.
    std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2), steps.end());
    const double step = steps[steps.size() / 2];
    bool regular = step > 0.0;
    for (double st : steps) regular = regular && st > 0.0 && std::abs(st - step) <= 0.01 * step;

    bool named = false;
    double unit = 1.0;
    if (sniff.header && !sniff.titles.empty())
    {
        const ChannelInfo info = parseColumnTitle(sniff.titles.front());
        const std::string name = lowerAscii(info.name);
        const std::string u = lowerAscii(info.unit);
        named = name == "t" || name == "s" || name == "ms" || name.rfind("temps", 0) == 0 || name.rfind("time", 0) == 0 ||
                name.rfind("horodatage", 0) == 0 || name == "seconds" || name == "secondes";
        if (u == "s" || u == "sec") named = true;
        else if (u == "ms") unit = 1e-3;
        else if (u == "us" || u == "\xC2\xB5s" || u == "\xCE\xBCs") unit = 1e-6;
        else if (u == "min") unit = 60.0;
        if (unit != 1.0) named = true;
        if (name == "ms" && u.empty()) unit = 1e-3;
        sniff.firstColumnIsTime = named;
    }
    else
    {
        sniff.firstColumnIsTime = regular;
    }
    if (sniff.firstColumnIsTime)
    {
        sniff.timeUnitSeconds = unit;
        sniff.timeStepSeconds = step > 0.0 ? step * unit : 0.0;
    }
    return sniff;
}

CsvSniff sniffCsv(const std::filesystem::path &file, std::size_t maximumLines)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return sniffCsv(in, maximumLines);
}
} // namespace qacq::model

