#include "qacq/plot/axis.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <system_error>

namespace qacq::plot
{
namespace
{
constexpr std::string_view kMinus = "\xE2\x88\x92"; // U+2212
constexpr std::string_view kDash = "\xE2\x80\x94";  // U+2014

struct PrefixEntry
{
    double factor;
    std::string_view symbol;
};
constexpr std::array<PrefixEntry, 9> kPrefixes = {{{1e-12, "p"},
                                                   {1e-9, "n"},
                                                   {1e-6, "\xC2\xB5"},
                                                   {1e-3, "m"},
                                                   {1.0, ""},
                                                   {1e3, "k"},
                                                   {1e6, "M"},
                                                   {1e9, "G"},
                                                   {1e12, "T"}}};

// Power of ten of the first significant digit (floor(log10)), robust to 999.9999 and 0.001.
int decade(double magnitude)
{
    return static_cast<int>(std::floor(std::log10(magnitude) + 1e-12));
}

double pow10(int exponent)
{
    return std::pow(10.0, static_cast<double>(exponent));
}

std::string superscript(int exponent)
{
    static constexpr std::array<std::string_view, 10> kDigits = {
        "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4",
        "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9"};
    std::string out;
    if (exponent < 0) out += "\xE2\x81\xBB"; // superscript minus
    const std::string digits = std::to_string(std::abs(exponent));
    for (char d : digits) out += kDigits[static_cast<std::size_t>(d - '0')];
    return out;
}

double roundTo(double value, int decimals)
{
    const double scale = pow10(decimals);
    return std::round(value * scale) / scale;
}

// Decimals that show `significant` digits of `value` (its rounding included: 9,9996 -> "10,00").
int decimalsFor(double value, int significant)
{
    const double magnitude = std::abs(value);
    if (!(magnitude > 0.0)) return std::max(0, significant - 1);
    int decimals = std::max(0, significant - 1 - decade(magnitude));
    const double rounded = std::abs(roundTo(value, decimals));
    if (rounded > 0.0 && decade(rounded) > decade(magnitude)) decimals = std::max(0, decimals - 1);
    return decimals;
}

std::string trimZeros(std::string text, char decimal)
{
    if (text.find(decimal) == std::string::npos) return text;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == decimal) text.pop_back();
    return text;
}

std::string withUnit(std::string number, std::string_view prefix, std::string_view unit)
{
    if (prefix.empty() && unit.empty()) return number;
    number += ' ';
    number += prefix;
    number += unit;
    return number;
}

int largestFor(std::string_view unit)
{
    return unit == "s" ? 0 : 4;
}

bool isSpaceAt(std::string_view s, std::size_t i, std::size_t &length)
{
    if (i >= s.size()) return false;
    if (s[i] == ' ' || s[i] == '\t')
    {
        length = 1;
        return true;
    }
    for (std::string_view space : {std::string_view("\xC2\xA0"), std::string_view("\xE2\x80\xAF"), std::string_view("\xE2\x80\x89")})
    {
        if (s.substr(i, space.size()) == space)
        {
            length = space.size();
            return true;
        }
    }
    return false;
}

std::string_view trim(std::string_view s)
{
    std::size_t length = 0;
    while (isSpaceAt(s, 0, length)) s.remove_prefix(length);
    for (;;)
    {
        bool removed = false;
        for (std::string_view space : {std::string_view(" "), std::string_view("\t"), std::string_view("\xC2\xA0"),
                                       std::string_view("\xE2\x80\xAF"), std::string_view("\xE2\x80\x89")})
        {
            if (s.size() >= space.size() && s.substr(s.size() - space.size()) == space)
            {
                s.remove_suffix(space.size());
                removed = true;
            }
        }
        if (!removed) return s;
    }
}

bool sameUnit(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        const auto la = ca < 128 ? static_cast<unsigned char>(std::tolower(ca)) : ca;
        const auto lb = cb < 128 ? static_cast<unsigned char>(std::tolower(cb)) : cb;
        if (la != lb) return false;
    }
    return true;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}
} // namespace

double niceStep(double minimum)
{
    if (!(minimum > 0.0) || !std::isfinite(minimum)) return 1.0;
    const int e = decade(minimum);
    const double base = pow10(e);
    const double m = minimum / base;
    if (m <= 1.0 + 1e-9) return base;
    if (m <= 2.0 + 1e-9) return 2.0 * base;
    if (m <= 5.0 + 1e-9) return 5.0 * base;
    return 10.0 * base;
}

Ticks linearTicks(double low, double high, double pixels, double spacing)
{
    Ticks ticks;
    if (!(high > low) || !std::isfinite(low) || !std::isfinite(high) || !(pixels > 0.0)) return ticks;
    const double step = niceStep((high - low) * std::max(1.0, spacing) / pixels);
    const double first = std::ceil(low / step - 1e-9);
    const double last = std::floor(high / step + 1e-9);
    if (!(last - first < 500.0)) return ticks;
    ticks.step = step;
    for (double k = first; k <= last; k += 1.0)
    {
        double v = k * step;
        if (std::abs(v) < step * 1e-9) v = 0.0;
        ticks.major.push_back(v);
    }
    // Minor ticks: fifths of a step of 1 or 5, quarters of a step of 2.
    const double mantissa = step / pow10(decade(step));
    const double sub = step / (std::abs(mantissa - 2.0) < 1e-6 ? 4.0 : 5.0);
    const double firstMinor = std::ceil(low / sub - 1e-9);
    const double lastMinor = std::floor(high / sub + 1e-9);
    if (lastMinor - firstMinor < 2500.0)
    {
        for (double k = firstMinor; k <= lastMinor; k += 1.0)
        {
            const double v = k * sub;
            const double r = v / step;
            if (std::abs(r - std::round(r)) < 1e-6) continue;
            ticks.minor.push_back(v);
        }
    }
    return ticks;
}

Ticks logTicks(double low, double high, double pixels, double spacing)
{
    Ticks ticks;
    if (!(low > 0.0) || !(high > low) || !std::isfinite(high) || !(pixels > 0.0)) return ticks;
    const double l0 = std::log10(low);
    const double l1 = std::log10(high);
    const double perDecade = pixels / (l1 - l0);
    const int d0 = static_cast<int>(std::floor(l0 - 1e-9));
    const int d1 = static_cast<int>(std::ceil(l1 + 1e-9));
    if (d1 - d0 > 400) return ticks;
    const int every = perDecade >= spacing ? 1 : static_cast<int>(std::ceil(spacing / perDecade));
    const bool between = perDecade >= 3.4 * spacing;
    const bool minors = perDecade >= 50.0;
    const auto inside = [&](double v) { return v >= low * (1.0 - 1e-9) && v <= high * (1.0 + 1e-9); };
    for (int d = d0; d <= d1; ++d)
    {
        const double base = pow10(d);
        if (((d % every) + every) % every == 0 && inside(base)) ticks.major.push_back(base);
        for (int m = 2; m <= 9; ++m)
        {
            const double v = m * base;
            if (!inside(v)) continue;
            if (between && (m == 2 || m == 5)) ticks.major.push_back(v);
            else if (minors) ticks.minor.push_back(v);
        }
    }
    std::sort(ticks.major.begin(), ticks.major.end());
    return ticks;
}

Prefix engineeringPrefix(double magnitude, int largest)
{
    if (!(magnitude > 0.0) || !std::isfinite(magnitude)) return {1.0, ""};
    int k = static_cast<int>(std::floor(std::log10(magnitude) / 3.0 + 1e-9));
    k = std::clamp(k, -4, std::clamp(largest, 0, 4));
    const PrefixEntry &entry = kPrefixes[static_cast<std::size_t>(k + 4)];
    return {entry.factor, entry.symbol};
}

bool takesPrefix(std::string_view unit)
{
    // SI units (and a few customary ones) that are written with a prefix. Anything else keeps its unit as
    // written: one already prefixed ("hPa", "kHz", "mA", "km"), decibels, "%", "\xC2\xB0C", "rpm",
    // "ppm", "LSB"... so a pressure in hPa never becomes "1,013 khPa".
    static constexpr std::array<std::string_view, 40> kPrefixable = {
        "s", "Hz", "V", "A", "W", "J", "N", "C", "F", "H", "T", "S", "Pa", "m", "g", "l", "L", "K", "mol", "cd",
        "Wb", "lx", "lm", "Gy", "Sv", "Bq", "eV", "bar", "Wh", "VA", "var", "Ah", "o", "B", "bit",
        "\xCE\xA9", "ohm", "m/s", "V/m", "A/m"};
    return std::find(kPrefixable.begin(), kPrefixable.end(), unit) != kPrefixable.end();
}

int significantFor(double value, double resolution, int minimum, int maximum)
{
    const double magnitude = std::abs(value);
    if (!(magnitude > 0.0) || !(resolution > 0.0) || !std::isfinite(magnitude) || !std::isfinite(resolution))
        return minimum;
    return std::clamp(decade(magnitude) - decade(resolution) + 1, minimum, maximum);
}

AxisLabels axisLabels(const Ticks &ticks, std::string_view unit, char decimal)
{
    AxisLabels out;
    out.unit = std::string(unit);
    if (ticks.major.empty()) return out;
    double magnitude = 0.0;
    for (double v : ticks.major) magnitude = std::max(magnitude, std::abs(v));
    double step = ticks.step;
    if (!(step > 0.0)) step = ticks.major.size() > 1 ? ticks.major[1] - ticks.major[0] : magnitude;
    if (!(magnitude > 0.0)) magnitude = step > 0.0 ? step : 1.0;
    if (takesPrefix(unit))
    {
        const Prefix p = engineeringPrefix(magnitude, largestFor(unit));
        out.factor = p.factor;
        out.unit = std::string(p.symbol) + std::string(unit);
    }
    else if (unit.empty() && (magnitude >= 1e6 || magnitude < 1e-3))
    {
        const int e3 = 3 * static_cast<int>(std::floor(std::log10(magnitude) / 3.0 + 1e-9));
        out.factor = pow10(e3);
        out.unit = "\xC3\x97" "10" + superscript(e3);
    }
    const double scaled = step / out.factor;
    out.decimals = scaled > 0.0 ? std::clamp(static_cast<int>(std::ceil(-std::log10(scaled) - 1e-9)), 0, 9) : 0;
    out.text.reserve(ticks.major.size());
    for (double v : ticks.major) out.text.push_back(formatFixed(v / out.factor, out.decimals, decimal));
    return out;
}

std::vector<std::string> logAxisLabels(const Ticks &ticks, std::string_view unit, char decimal)
{
    std::vector<std::string> text;
    text.reserve(ticks.major.size());
    for (double v : ticks.major) text.push_back(formatCompact(v, unit, 3, decimal));
    return text;
}

std::string formatFixed(double value, int decimals, char decimal)
{
    if (!std::isfinite(value)) return std::string(kDash);
    decimals = std::clamp(decimals, 0, 17);
    std::array<char, 400> buffer{};
    const double magnitude = std::abs(value);
    std::to_chars_result result{};
    if (magnitude < 1e21) result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, decimals);
    else result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific, 3);
    if (result.ec != std::errc()) return std::string(kDash);
    std::string text(buffer.data(), result.ptr);
    bool zero = true;
    for (char c : text)
    {
        if (c >= '1' && c <= '9') zero = false;
    }
    std::string out;
    out.reserve(text.size() + 2);
    for (char c : text)
    {
        if (c == '-')
        {
            if (!zero) out += kMinus;
        }
        else if (c == '.')
        {
            out += decimal;
        }
        else
        {
            out += c;
        }
    }
    return out;
}

std::string formatQuantity(double value, std::string_view unit, int significant, char decimal)
{
    if (!std::isfinite(value)) return std::string(kDash);
    significant = std::clamp(significant, 1, 15);
    if (value == 0.0) return withUnit("0", "", unit);
    const double magnitude = std::abs(value);
    if (takesPrefix(unit))
    {
        const int largest = largestFor(unit);
        Prefix p = engineeringPrefix(magnitude, largest);
        double mantissa = value / p.factor;
        int decimals = decimalsFor(mantissa, significant);
        // 999,96 rounded to four digits is 1 000,0: the next prefix reads better ("1,000 k").
        if (std::abs(roundTo(mantissa, decimals)) >= 1000.0 && p.factor < 1e12 && p.factor < pow10(3 * largest))
        {
            p = engineeringPrefix(magnitude * 1.001, largest);
            mantissa = value / p.factor;
            decimals = decimalsFor(mantissa, significant);
        }
        return withUnit(formatFixed(mantissa, decimals, decimal), p.symbol, unit);
    }
    if (unit.empty() && (magnitude >= 1e6 || magnitude < 1e-3))
    {
        int e = decade(magnitude);
        double mantissa = value / pow10(e);
        int decimals = significant - 1;
        if (std::abs(roundTo(mantissa, decimals)) >= 10.0)
        {
            ++e;
            mantissa = value / pow10(e);
        }
        return formatFixed(mantissa, decimals, decimal) + "\xC3\x97" "10" + superscript(e);
    }
    return withUnit(formatFixed(value, decimalsFor(value, significant), decimal), "", unit);
}

std::string formatCompact(double value, std::string_view unit, int significant, char decimal)
{
    if (!std::isfinite(value)) return std::string(kDash);
    if (value == 0.0) return withUnit("0", "", unit);
    const double magnitude = std::abs(value);
    if (takesPrefix(unit))
    {
        const Prefix p = engineeringPrefix(magnitude * 1.0000001, largestFor(unit));
        const double mantissa = value / p.factor;
        return withUnit(trimZeros(formatFixed(mantissa, decimalsFor(mantissa, significant), decimal), decimal), p.symbol, unit);
    }
    return withUnit(trimZeros(formatFixed(value, decimalsFor(value, significant), decimal), decimal), "", unit);
}

Parsed parseQuantity(std::string_view text, std::string_view unit, double bareFactor)
{
    Parsed out;
    const std::string_view s = trim(text);
    if (s.empty())
    {
        out.error = "Valeur vide.";
        return out;
    }
    std::string number;
    std::size_t i = 0;
    if (s[0] == '-' || s[0] == '+')
    {
        if (s[0] == '-') number += '-';
        i = 1;
    }
    else if (s.substr(0, kMinus.size()) == kMinus)
    {
        number += '-';
        i = kMinus.size();
    }
    bool digits = false;
    bool point = false;
    for (;;)
    {
        std::size_t length = 0;
        if (i < s.size() && isDigit(s[i]))
        {
            number += s[i++];
            digits = true;
        }
        else if (i < s.size() && (s[i] == '.' || s[i] == ',') && !point)
        {
            number += '.';
            point = true;
            ++i;
        }
        else if (digits && !point && isSpaceAt(s, i, length) && i + length + 3 <= s.size() && isDigit(s[i + length]) &&
                 isDigit(s[i + length + 1]) && isDigit(s[i + length + 2]) &&
                 (i + length + 3 == s.size() || !isDigit(s[i + length + 3])))
        {
            i += length; // a space between thousands: "1 200"
        }
        else
        {
            break;
        }
    }
    if (!digits)
    {
        out.error = unit.empty() ? "Nombre attendu." : "Nombre attendu (exemple : 2 k" + std::string(unit) + ").";
        return out;
    }
    if (i + 1 < s.size() && (s[i] == 'e' || s[i] == 'E'))
    {
        std::size_t j = i + 1;
        std::string exponent = "e";
        if (s[j] == '+' || s[j] == '-') exponent += s[j++];
        if (j < s.size() && isDigit(s[j]))
        {
            while (j < s.size() && isDigit(s[j])) exponent += s[j++];
            number += exponent;
            i = j;
        }
    }
    double value = 0.0;
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (ec != std::errc() || end != number.data() + number.size() || !std::isfinite(value))
    {
        out.error = "Nombre illisible.";
        return out;
    }
    const std::string_view rest = trim(s.substr(i));
    if (rest.empty())
    {
        out.ok = true;
        out.value = value * bareFactor;
        return out;
    }
    if (!unit.empty() && sameUnit(rest, unit))
    {
        out.ok = true;
        out.value = value;
        return out;
    }
    if (takesPrefix(unit) || unit.empty())
    {
        struct Written
        {
            std::string_view symbol;
            double factor;
        };
        static constexpr std::array<Written, 11> kWritten = {{{"p", 1e-12},
                                                              {"n", 1e-9},
                                                              {"u", 1e-6},
                                                              {"\xC2\xB5", 1e-6},
                                                              {"\xCE\xBC", 1e-6},
                                                              {"m", 1e-3},
                                                              {"k", 1e3},
                                                              {"K", 1e3},
                                                              {"M", 1e6},
                                                              {"G", 1e9},
                                                              {"T", 1e12}}};
        for (const Written &w : kWritten)
        {
            if (rest.substr(0, w.symbol.size()) != w.symbol) continue;
            const std::string_view after = trim(rest.substr(w.symbol.size()));
            if (after.empty() || sameUnit(after, unit))
            {
                out.ok = true;
                out.value = value * w.factor;
                return out;
            }
        }
    }
    out.error = unit.empty() ? "« " + std::string(rest) + " » : un nombre seul est attendu ici."
                             : "« " + std::string(rest) + " » n'est pas une unité de " + std::string(unit) + ".";
    return out;
}
} // namespace qacq::plot
