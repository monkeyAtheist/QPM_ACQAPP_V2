#include "qacq/model/json.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace qacq::model
{
namespace
{
const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;
} // namespace

Json::Json(Array value) : m_value(std::move(value)) {}
Json::Json(Object value) : m_value(std::move(value)) {}

Json Json::array()
{
    return Json(Array{});
}

Json Json::object()
{
    return Json(Object{});
}

bool Json::toBool(bool fallback) const noexcept
{
    const bool *b = std::get_if<bool>(&m_value);
    return b ? *b : fallback;
}

double Json::toNumber(double fallback) const noexcept
{
    const double *d = std::get_if<double>(&m_value);
    return d ? *d : fallback;
}

std::string Json::toString(std::string fallback) const
{
    const std::string *s = std::get_if<std::string>(&m_value);
    return s ? *s : fallback;
}

const Json::Array &Json::items() const noexcept
{
    const Array *a = std::get_if<Array>(&m_value);
    return a ? *a : kEmptyArray;
}

const Json::Object &Json::members() const noexcept
{
    const Object *o = std::get_if<Object>(&m_value);
    return o ? *o : kEmptyObject;
}

const Json &Json::operator[](std::string_view key) const noexcept
{
    for (const JsonMember &m : members())
        if (m.key == key) return m.value;
    return kNull;
}

Json &Json::operator[](std::string_view key)
{
    if (isNull()) m_value = Object{};
    Object *o = std::get_if<Object>(&m_value);
    if (!o)
    {
        m_value = Object{};
        o = std::get_if<Object>(&m_value);
    }
    for (JsonMember &m : *o)
        if (m.key == key) return m.value;
    o->push_back({std::string(key), Json()});
    return o->back().value;
}

bool Json::contains(std::string_view key) const noexcept
{
    for (const JsonMember &m : members())
        if (m.key == key) return true;
    return false;
}

bool Json::erase(std::string_view key)
{
    Object *o = std::get_if<Object>(&m_value);
    if (!o) return false;
    for (auto it = o->begin(); it != o->end(); ++it)
    {
        if (it->key == key)
        {
            o->erase(it);
            return true;
        }
    }
    return false;
}

void Json::push_back(Json value)
{
    if (!isArray()) m_value = Array{};
    std::get<Array>(m_value).push_back(std::move(value));
}

const Json &Json::at(std::size_t index) const noexcept
{
    const Array &a = items();
    return index < a.size() ? a[index] : kNull;
}

std::size_t Json::size() const noexcept
{
    if (const Array *a = std::get_if<Array>(&m_value)) return a->size();
    if (const Object *o = std::get_if<Object>(&m_value)) return o->size();
    return 0;
}

bool Json::operator==(const Json &other) const
{
    return m_value == other.m_value;
}

// ------------------------------------------------------------------------------------------- writing
std::string formatNumber(double value)
{
    if (!std::isfinite(value)) return "null";
    if (value == 0.0) return "0"; // also -0
    std::array<char, 32> buffer{};
    const auto r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), r.ptr);
}

namespace
{
void writeString(std::string &out, std::string_view s)
{
    out.push_back('"');
    for (char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                }
                else
                {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void newline(std::string &out, int indent, int level)
{
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent * level), ' ');
}
} // namespace

void Json::write(std::string &out, int indent, int level) const
{
    switch (type())
    {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(m_value) ? "true" : "false"; break;
        case Type::Number: out += formatNumber(std::get<double>(m_value)); break;
        case Type::String: writeString(out, std::get<std::string>(m_value)); break;
        case Type::Array:
        {
            const Array &a = std::get<Array>(m_value);
            if (a.empty())
            {
                out += "[]";
                break;
            }
            // Short arrays of numbers stay on one line, even when pretty-printing.
            bool flat = indent < 0 || a.size() <= 16;
            for (const Json &v : a) flat = flat && (v.isNumber() || v.isBool() || v.isNull());
            out.push_back('[');
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (i) out += indent >= 0 && flat ? ", " : ",";
                if (indent >= 0 && !flat) newline(out, indent, level + 1);
                a[i].write(out, indent, level + 1);
            }
            if (indent >= 0 && !flat) newline(out, indent, level);
            out.push_back(']');
            break;
        }
        case Type::Object:
        {
            const Object &o = std::get<Object>(m_value);
            if (o.empty())
            {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < o.size(); ++i)
            {
                if (i) out.push_back(',');
                if (indent >= 0) newline(out, indent, level + 1);
                writeString(out, o[i].key);
                out += indent >= 0 ? ": " : ":";
                o[i].value.write(out, indent, level + 1);
            }
            if (indent >= 0) newline(out, indent, level);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const
{
    std::string out;
    write(out, indent, 0);
    return out;
}

// ------------------------------------------------------------------------------------------- parsing
namespace
{
class Parser
{
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::optional<Json> run(std::string *error)
    {
        skipSpace();
        Json value;
        if (!parseValue(value, 0))
        {
            if (error) *error = message();
            return std::nullopt;
        }
        skipSpace();
        if (m_pos != m_text.size())
        {
            fail("caractères en trop après la valeur");
            if (error) *error = message();
            return std::nullopt;
        }
        return value;
    }

private:
    bool fail(const char *what)
    {
        if (m_error.empty()) m_error = what;
        return false;
    }

    std::string message() const
    {
        // Line and column of the error, counted from 1.
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < m_pos && i < m_text.size(); ++i)
        {
            if (m_text[i] == '\n')
            {
                ++line;
                column = 1;
            }
            else
            {
                ++column;
            }
        }
        return "JSON invalide (ligne " + std::to_string(line) + ", colonne " + std::to_string(column) + ") : " + m_error;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
            ++m_pos;
    }

    bool literal(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word) return fail("valeur inconnue");
        m_pos += word.size();
        return true;
    }

    bool parseValue(Json &value, int depth)
    {
        if (depth > 256) return fail("imbrication trop profonde");
        if (m_pos >= m_text.size()) return fail("fin de texte inattendue");
        const char c = m_text[m_pos];
        switch (c)
        {
            case 'n':
                value = Json();
                return literal("null");
            case 't':
                value = Json(true);
                return literal("true");
            case 'f':
                value = Json(false);
                return literal("false");
            case '"':
            {
                std::string s;
                if (!parseString(s)) return false;
                value = Json(std::move(s));
                return true;
            }
            case '[': return parseArray(value, depth);
            case '{': return parseObject(value, depth);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(value);
                return fail("valeur attendue");
        }
    }

    bool parseNumber(Json &value)
    {
        const std::size_t start = m_pos;
        auto digit = [&] { return m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9'; };
        if (m_text[m_pos] == '-') ++m_pos;
        if (!digit()) return fail("chiffre attendu");
        if (m_text[m_pos] == '0')
        {
            ++m_pos;
            if (digit()) return fail("zéro en tête d'un nombre");
        }
        else
        {
            while (digit()) ++m_pos;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.')
        {
            ++m_pos;
            if (!digit()) return fail("chiffre attendu après le point");
            while (digit()) ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
        {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) ++m_pos;
            if (!digit()) return fail("chiffre attendu dans l'exposant");
            while (digit()) ++m_pos;
        }
        double v = 0.0;
        const char *first = m_text.data() + start;
        const char *last = m_text.data() + m_pos;
        const auto r = std::from_chars(first, last, v);
        if (r.ec == std::errc::result_out_of_range)
        {
            // Too large (keep the largest double: infinity is not JSON) or too small (0). Decimal
            // magnitude = significant integer digits (or minus the zeros after the point) + exponent.
            std::string_view t(first, static_cast<std::size_t>(last - first));
            const bool negative = !t.empty() && t.front() == '-';
            if (negative) t.remove_prefix(1);
            const std::size_t e = t.find_first_of("eE");
            const std::string_view mantissa = t.substr(0, e);
            long exponent = 0;
            if (e != std::string_view::npos)
            {
                std::string_view x = t.substr(e + 1);
                if (!x.empty() && x.front() == '+') x.remove_prefix(1);
                const bool negativeExponent = !x.empty() && x.front() == '-';
                if (std::from_chars(x.data(), x.data() + x.size(), exponent).ec != std::errc())
                    exponent = negativeExponent ? -1000000 : 1000000; // more digits than a long holds
            }
            const std::size_t point = mantissa.find('.');
            const std::string_view whole = mantissa.substr(0, point);
            long magnitude = 0;
            const std::size_t firstSignificant = whole.find_first_not_of('0');
            if (firstSignificant != std::string_view::npos)
                magnitude = static_cast<long>(whole.size() - firstSignificant);
            else if (point != std::string_view::npos)
                magnitude = -static_cast<long>(mantissa.substr(point + 1).find_first_not_of('0'));
            v = magnitude + exponent > 0 ? std::numeric_limits<double>::max() : 0.0;
            if (negative) v = -v;
        }
        else if (r.ec != std::errc() || r.ptr != last)
        {
            return fail("nombre illisible");
        }
        value = Json(v);
        return true;
    }

    static void appendUtf8(std::string &out, std::uint32_t cp)
    {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(std::uint32_t &cp)
    {
        if (m_pos + 4 > m_text.size()) return fail("échappement \\u incomplet");
        cp = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char h = m_text[m_pos++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return fail("chiffre hexadécimal attendu");
        }
        return true;
    }

    bool parseString(std::string &out)
    {
        ++m_pos; // opening quote
        while (true)
        {
            if (m_pos >= m_text.size()) return fail("chaîne non terminée");
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("caractère de contrôle dans une chaîne");
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) return fail("chaîne non terminée");
            const char e = m_text[m_pos++];
            switch (e)
            {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        // High surrogate: a low one must follow.
                        if (m_text.substr(m_pos, 2) != "\\u") return fail("paire de substitution incomplète");
                        m_pos += 2;
                        std::uint32_t low = 0;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail("paire de substitution invalide");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    {
                        return fail("paire de substitution invalide");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("échappement inconnu");
            }
        }
    }

    bool parseArray(Json &value, int depth)
    {
        ++m_pos;
        Json::Array items;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']')
        {
            ++m_pos;
            value = Json(std::move(items));
            return true;
        }
        while (true)
        {
            skipSpace();
            Json item;
            if (!parseValue(item, depth + 1)) return false;
            items.push_back(std::move(item));
            skipSpace();
            if (m_pos >= m_text.size()) return fail("tableau non terminé");
            const char c = m_text[m_pos++];
            if (c == ']') break;
            if (c != ',') return fail("',' ou ']' attendu");
        }
        value = Json(std::move(items));
        return true;
    }

    bool parseObject(Json &value, int depth)
    {
        ++m_pos;
        Json::Object members;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}')
        {
            ++m_pos;
            value = Json(std::move(members));
            return true;
        }
        while (true)
        {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') return fail("nom de membre attendu");
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') return fail("':' attendu");
            ++m_pos;
            skipSpace();
            Json item;
            if (!parseValue(item, depth + 1)) return false;
            members.push_back({std::move(key), std::move(item)});
            skipSpace();
            if (m_pos >= m_text.size()) return fail("objet non terminé");
            const char c = m_text[m_pos++];
            if (c == '}') break;
            if (c != ',') return fail("',' ou '}' attendu");
        }
        value = Json(std::move(members));
        return true;
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
    std::string m_error;
};
} // namespace

std::optional<Json> Json::parse(std::string_view text, std::string *error)
{
    return Parser(text).run(error);
}
} // namespace qacq::model
