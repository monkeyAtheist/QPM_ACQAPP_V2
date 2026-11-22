#include "qacq/analysis/expression.h"

#include "qacq/dsp/constants.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace qacq::analysis
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = dsp::kPi;
constexpr std::size_t kLongest = 4000; // characters of a formula
constexpr std::size_t kMostNodes = 4000;
constexpr int kDeepest = 100; // nested parentheses and calls

enum Kind
{
    Number,
    Name,
    Negate,
    Binary,
    Call,
    If
};

enum Operator
{
    Plus,
    Minus,
    Times,
    Divide,
    Power,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual
};

enum Function
{
    Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh, Sqrt, Abs, Exp, Ln, Log10, Log2, Floor, Ceil,
    Round, Sign, Min, Max, Mod, Pow, Sinc, Odd, Even, Sind, Cosd, Tand, Deg, Rad, IfThenElse
};

struct FunctionInfo
{
    std::string_view name;
    Function function;
    std::size_t fewest, most; // arguments
};

constexpr FunctionInfo kFunctions[] = {
    {"sin", Sin, 1, 1},       {"cos", Cos, 1, 1},     {"tan", Tan, 1, 1},     {"asin", Asin, 1, 1},
    {"acos", Acos, 1, 1},     {"atan", Atan, 1, 1},   {"atan2", Atan2, 2, 2}, {"sinh", Sinh, 1, 1},
    {"cosh", Cosh, 1, 1},     {"tanh", Tanh, 1, 1},   {"sqrt", Sqrt, 1, 1},   {"abs", Abs, 1, 1},
    {"exp", Exp, 1, 1},       {"ln", Ln, 1, 1},       {"log", Log10, 1, 1},   {"log10", Log10, 1, 1},
    {"log2", Log2, 1, 1},     {"floor", Floor, 1, 1}, {"ceil", Ceil, 1, 1},   {"round", Round, 1, 1},
    {"sign", Sign, 1, 1},     {"min", Min, 2, 64},    {"max", Max, 2, 64},    {"mod", Mod, 2, 2},
    {"pow", Pow, 2, 2},       {"sinc", Sinc, 1, 1},   {"odd", Odd, 1, 1},     {"even", Even, 1, 1},
    {"sind", Sind, 1, 1},     {"cosd", Cosd, 1, 1},   {"tand", Tand, 1, 1},   {"deg", Deg, 1, 1},
    {"rad", Rad, 1, 1},       {"if", IfThenElse, 3, 3}};

const FunctionInfo *findFunction(std::string_view name) noexcept
{
    for (const FunctionInfo &f : kFunctions)
        if (f.name == name) return &f;
    return nullptr;
}

bool isAsciiLetter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// Symbols of more than one byte the formulas know (UTF-8), and the spaces typed in French texts.
constexpr std::string_view kTimes = "\xC3\x97";      // ×
constexpr std::string_view kDot = "\xC2\xB7";        // ·
constexpr std::string_view kDivide = "\xC3\xB7";     // ÷
constexpr std::string_view kMinus = "\xE2\x88\x92";  // −
constexpr std::string_view kLessEq = "\xE2\x89\xA4"; // ≤
constexpr std::string_view kMoreEq = "\xE2\x89\xA5"; // ≥
constexpr std::string_view kNotEq = "\xE2\x89\xA0";  // ≠
constexpr std::string_view kPiSign = "\xCF\x80";     // π
constexpr std::string_view kSpaces[] = {"\xC2\xA0", "\xE2\x80\xAF", "\xE2\x80\x89"};
constexpr std::string_view kSymbols[] = {kTimes, kDot, kDivide, kMinus, kLessEq, kMoreEq, kNotEq};

// Length of the UTF-8 sequence that starts with this byte.
std::size_t sequenceLength(unsigned char c) noexcept
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::size_t characterCount(std::string_view text) noexcept
{
    std::size_t count = 0;
    for (char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
    return count;
}

class Parser
{
public:
    Parser(std::string_view text, std::span<const std::string> names, std::vector<Expression::Node> &nodes)
        : m_s(text), m_names(names), m_nodes(nodes)
    {
    }

    int parse(std::string *error)
    {
        if (m_s.size() > kLongest)
            fail("Formule trop longue (" + std::to_string(kLongest) + " caractères au plus).", 0);
        skip();
        if (!failed() && m_i >= m_s.size()) fail("Formule vide.", 0);
        int root = failed() ? -1 : comparison();
        skip();
        if (!failed() && m_i < m_s.size())
        {
            if (m_s[m_i] == ')') fail("Parenthèse fermante « ) » de trop.", m_i);
            else if (m_s[m_i] == ';' || m_s[m_i] == ',')
                fail("« " + std::string(1, m_s[m_i]) + " » hors d'une fonction (pour une virgule décimale, pas d'espace : 0,5).", m_i);
            else fail("Il manque un opérateur avant « " + piece(m_i) + " ».", m_i);
        }
        if (failed())
        {
            if (error) *error = m_error + " (caractère " + std::to_string(characterCount(m_s.substr(0, m_errorAt)) + 1) + ")";
            return -1;
        }
        if (error) error->clear();
        return root;
    }

private:
    using Node = Expression::Node;

    void fail(std::string message, std::size_t at)
    {
        if (!m_error.empty()) return;
        m_error = std::move(message);
        m_errorAt = at;
    }
    bool failed() const noexcept { return !m_error.empty(); }

    // A few characters from `at`, for the messages.
    std::string piece(std::size_t at) const
    {
        std::size_t end = at;
        for (int k = 0; k < 6 && end < m_s.size(); ++k) end += sequenceLength(static_cast<unsigned char>(m_s[end]));
        return std::string(m_s.substr(at, std::min(end, m_s.size()) - at));
    }

    bool startsWith(std::string_view token) const noexcept { return m_s.substr(m_i).starts_with(token); }

    void skip() noexcept
    {
        for (;;)
        {
            if (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\r' || m_s[m_i] == '\n'))
            {
                ++m_i;
                continue;
            }
            bool spaced = false;
            for (std::string_view space : kSpaces)
            {
                if (startsWith(space))
                {
                    m_i += space.size();
                    spaced = true;
                    break;
                }
            }
            if (!spaced) return;
        }
    }

    bool match(std::string_view token)
    {
        skip();
        if (!startsWith(token)) return false;
        m_i += token.size();
        return true;
    }

    bool symbolAt(std::size_t at) const noexcept
    {
        const std::string_view rest = m_s.substr(at);
        for (std::string_view symbol : kSymbols)
            if (rest.starts_with(symbol)) return true;
        for (std::string_view space : kSpaces)
            if (rest.starts_with(space)) return true;
        return false;
    }

    bool identifierStart(std::size_t at) const noexcept
    {
        if (at >= m_s.size()) return false;
        const auto c = static_cast<unsigned char>(m_s[at]);
        return isAsciiLetter(static_cast<char>(c)) || c == '_' || (c >= 0x80 && !symbolAt(at));
    }

    int add(Node node)
    {
        if (m_nodes.size() >= kMostNodes)
        {
            fail("Formule trop longue.", m_i);
            return -1;
        }
        m_nodes.push_back(std::move(node));
        return static_cast<int>(m_nodes.size()) - 1;
    }

    int binary(Operator op, int left, int right)
    {
        if (failed()) return -1;
        Node n;
        n.kind = Binary;
        n.op = op;
        n.a = left;
        n.b = right;
        return add(std::move(n));
    }

    int comparison()
    {
        const int left = additive();
        if (failed()) return -1;
        skip();
        const auto compare = [this]() -> int {
            if (match("<=") || match(kLessEq)) return LessEqual;
            if (match(">=") || match(kMoreEq)) return GreaterEqual;
            if (match("==")) return Equal;
            if (match("!=") || match(kNotEq) || match("<>")) return NotEqual;
            if (match("<")) return Less;
            if (match(">")) return Greater;
            if (match("=")) return Equal;
            return -1;
        };
        const int op = compare();
        if (op < 0) return left;
        const int right = additive();
        if (failed()) return -1;
        const std::size_t at = m_i;
        if (compare() >= 0)
        {
            fail("Une seule comparaison à la fois : (a < b) * (b < c).", at);
            return -1;
        }
        return binary(static_cast<Operator>(op), left, right);
    }

    int additive()
    {
        int left = multiplicative();
        while (!failed())
        {
            if (match("+")) left = binary(Plus, left, multiplicative());
            else if (match("-") || match(kMinus)) left = binary(Minus, left, multiplicative());
            else break;
        }
        return failed() ? -1 : left;
    }

    int multiplicative()
    {
        bool closed = false; // the operand ended with a number or ")": a product may follow, implicit
        int left = unary(closed);
        while (!failed())
        {
            skip();
            if (match("**"))
            {
                fail("La puissance s'écrit « ^ » : 2^3.", m_i - 2);
                break;
            }
            if (match("*") || match(kTimes) || match(kDot)) left = binary(Times, left, unary(closed));
            else if (match("/") || match(kDivide)) left = binary(Divide, left, unary(closed));
            else if (closed && (identifierStart(m_i) || (m_i < m_s.size() && m_s[m_i] == '(')))
                left = binary(Times, left, unary(closed));
            else if (closed && m_i < m_s.size() && isDigit(m_s[m_i]))
            {
                fail("Il manque un opérateur avant « " + piece(m_i) + " ».", m_i);
                break;
            }
            else break;
        }
        return failed() ? -1 : left;
    }

    int unary(bool &closed)
    {
        skip();
        if (match("-") || match(kMinus))
        {
            const int operand = unary(closed);
            if (failed()) return -1;
            Node n;
            n.kind = Negate;
            n.a = operand;
            return add(std::move(n));
        }
        if (match("+")) return unary(closed);
        return power(closed);
    }

    int power(bool &closed)
    {
        const int base = primary(closed);
        if (failed()) return -1;
        if (match("^"))
        {
            const int exponent = unary(closed); // right to left, and 2^-1
            return binary(Power, base, exponent);
        }
        return base;
    }

    int number(bool &closed)
    {
        const std::size_t start = m_i;
        std::string digits;
        while (m_i < m_s.size() && isDigit(m_s[m_i])) digits += m_s[m_i++];
        if (m_i + 1 < m_s.size() && (m_s[m_i] == '.' || m_s[m_i] == ',') && isDigit(m_s[m_i + 1]))
        {
            digits += '.';
            ++m_i;
            while (m_i < m_s.size() && isDigit(m_s[m_i])) digits += m_s[m_i++];
        }
        else if (m_i < m_s.size() && m_s[m_i] == '.' && !digits.empty())
        {
            ++m_i; // "2." is 2
        }
        if (digits.empty() || digits == ".")
        {
            fail("Nombre attendu.", start);
            return -1;
        }
        if (digits.front() == '.') digits.insert(digits.begin(), '0');
        // An exponent only when digits follow: "2e" is 2 times e.
        if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E'))
        {
            std::size_t j = m_i + 1;
            if (j < m_s.size() && (m_s[j] == '+' || m_s[j] == '-')) ++j;
            if (j < m_s.size() && isDigit(m_s[j]))
            {
                digits += 'e';
                digits.append(m_s.substr(m_i + 1, j - m_i - 1));
                m_i = j;
                while (m_i < m_s.size() && isDigit(m_s[m_i])) digits += m_s[m_i++];
            }
        }
        double value = 0.0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc() || end != digits.data() + digits.size() || !std::isfinite(value))
        {
            fail("Nombre illisible « " + std::string(m_s.substr(start, m_i - start)) + " ».", start);
            return -1;
        }
        closed = true;
        Node n;
        n.kind = Number;
        n.value = value;
        return add(std::move(n));
    }

    std::string identifier()
    {
        const std::size_t start = m_i;
        while (m_i < m_s.size())
        {
            const auto c = static_cast<unsigned char>(m_s[m_i]);
            if (isAsciiLetter(static_cast<char>(c)) || isDigit(static_cast<char>(c)) || c == '_')
                ++m_i;
            else if (c >= 0x80 && !symbolAt(m_i))
                m_i += sequenceLength(c);
            else
                break;
        }
        m_i = std::min(m_i, m_s.size());
        return std::string(m_s.substr(start, m_i - start));
    }

    int call(const std::string &name, std::size_t at, bool &closed)
    {
        const FunctionInfo *f = findFunction(name);
        if (!f)
        {
            const bool known = std::find(m_names.begin(), m_names.end(), name) != m_names.end() || name == "pi" ||
                               name == kPiSign || name == "e";
            fail(known ? "« " + name + " » n'est pas une fonction : un « * » manque-t-il ? (" + name + "*(…))"
                       : "Fonction inconnue « " + name + " ».",
                 at);
            return -1;
        }
        ++m_i; // (
        std::vector<int> args;
        bool decimalComma = false; // a comma between digits in the arguments: perhaps meant as a separator
        skip();
        if (!match(")"))
        {
            for (;;)
            {
                const std::size_t from = m_i;
                args.push_back(comparison());
                if (failed()) return -1;
                for (std::size_t k = from + 1; k + 1 < m_i && k < m_s.size(); ++k)
                    if (m_s[k] == ',' && isDigit(m_s[k - 1]) && isDigit(m_s[k + 1])) decimalComma = true;
                if (match(";") || match(",")) continue;
                if (match(")")) break;
                fail("« ; » ou « ) » attendu dans les arguments de " + name + ".", m_i);
                return -1;
            }
        }
        if (args.size() < f->fewest || args.size() > f->most)
        {
            std::string expected = f->fewest == f->most ? std::to_string(f->fewest) : "au moins " + std::to_string(f->fewest);
            std::string message = name + " attend " + expected + (f->fewest > 1 ? " arguments" : " argument");
            if (decimalComma && args.size() < f->fewest)
                message += " : séparez-les par « ; » (la virgule entre deux chiffres est décimale)";
            fail(message + ".", at);
            return -1;
        }
        closed = true;
        Node n;
        if (f->function == IfThenElse)
        {
            n.kind = If;
            n.a = args[0];
            n.b = args[1];
            n.c = args[2];
        }
        else
        {
            n.kind = Call;
            n.op = f->function;
            n.args = std::move(args);
        }
        return add(std::move(n));
    }

    int primary(bool &closed)
    {
        closed = false;
        skip();
        struct Leave
        {
            int &depth;
            ~Leave() { --depth; }
        } leave{++m_depth};
        if (m_depth > kDeepest)
        {
            fail("Formule trop imbriquée.", m_i);
            return -1;
        }
        if (m_i >= m_s.size())
        {
            fail("Formule incomplète : une valeur manque à la fin.", m_i);
            return -1;
        }
        const char c = m_s[m_i];
        if (c == '(')
        {
            ++m_i;
            const int inner = comparison();
            if (failed()) return -1;
            if (!match(")"))
            {
                fail("Il manque une parenthèse fermante « ) ».", m_i);
                return -1;
            }
            closed = true;
            return inner;
        }
        if (isDigit(c) || ((c == '.' || c == ',') && m_i + 1 < m_s.size() && isDigit(m_s[m_i + 1]))) return number(closed);
        if (identifierStart(m_i))
        {
            const std::size_t at = m_i;
            const std::string name = identifier();
            skip();
            if (m_i < m_s.size() && m_s[m_i] == '(') return call(name, at, closed);
            if (findFunction(name))
            {
                fail("« " + name + " » est une fonction : " + name + "(…).", at);
                return -1;
            }
            // A name may be followed by an implicit product too ("pi n", "E n"; "n(" is a call).
            closed = true;
            Node n;
            if (name == "pi" || name == kPiSign)
            {
                n.kind = Number;
                n.value = kPi;
                return add(std::move(n));
            }
            const auto it = std::find(m_names.begin(), m_names.end(), name);
            if (it != m_names.end())
            {
                n.kind = Name;
                n.op = static_cast<int>(it - m_names.begin());
                return add(std::move(n));
            }
            if (name == "e")
            {
                n.kind = Number;
                n.value = std::exp(1.0);
                return add(std::move(n));
            }
            fail(name == "t" ? std::string("Ces formules ne dépendent pas du temps : « t » n'existe pas ici.")
                             : "Nom inconnu « " + name + " » : déclarez-le dans les variables (" + name + " = 1).",
                 at);
            return -1;
        }
        if (c == ')') fail("Une valeur manque avant « ) ».", m_i);
        else if (symbolAt(m_i) || c == '*' || c == '/' || c == '^' || c == '<' || c == '>' || c == '=')
            fail("Une valeur manque avant « " + piece(m_i).substr(0, sequenceLength(static_cast<unsigned char>(c))) + " ».", m_i);
        else
            fail("Caractère inattendu « " + std::string(m_s.substr(m_i, sequenceLength(static_cast<unsigned char>(c)))) + " ».", m_i);
        return -1;
    }

    std::string_view m_s;
    std::size_t m_i = 0;
    std::span<const std::string> m_names;
    std::vector<Node> &m_nodes;
    std::string m_error;
    std::size_t m_errorAt = 0;
    int m_depth = 0;
};

std::string_view trim(std::string_view s) noexcept
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

bool validName(std::string_view name) noexcept
{
    if (name.empty()) return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (!(isAsciiLetter(static_cast<char>(first)) || first == '_' || first >= 0x80)) return false;
    for (char c : name)
    {
        const auto u = static_cast<unsigned char>(c);
        if (!(isAsciiLetter(c) || isDigit(c) || c == '_' || u >= 0x80)) return false;
    }
    for (std::string_view symbol : kSymbols)
        if (name.find(symbol) != std::string_view::npos) return false;
    return true;
}
} // namespace

Expression Expression::parse(std::string_view text, std::span<const std::string> names, std::string *error)
{
    Expression e;
    Parser parser(text, names, e.m_nodes);
    e.m_root = parser.parse(error);
    if (e.m_root < 0) e.m_nodes.clear();
    return e;
}

double Expression::evaluate(std::span<const double> values) const noexcept
{
    if (m_root < 0) return kNaN;
    return eval(m_root, values, 0);
}

bool Expression::uses(std::size_t name) const noexcept
{
    return std::any_of(m_nodes.begin(), m_nodes.end(),
                       [name](const Node &n) { return n.kind == Name && static_cast<std::size_t>(n.op) == name; });
}

double Expression::eval(int index, std::span<const double> values, int depth) const noexcept
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_nodes.size() || depth > static_cast<int>(kMostNodes)) return kNaN;
    const Node &n = m_nodes[static_cast<std::size_t>(index)];
    const auto arg = [&](int k) { return eval(k, values, depth + 1); };
    switch (n.kind)
    {
        case Number: return n.value;
        case Name: return static_cast<std::size_t>(n.op) < values.size() ? values[static_cast<std::size_t>(n.op)] : kNaN;
        case Negate: return -arg(n.a);
        case If:
        {
            const double condition = arg(n.a);
            if (std::isnan(condition)) return kNaN;
            return condition != 0.0 ? arg(n.b) : arg(n.c);
        }
        case Binary:
        {
            const double a = arg(n.a), b = arg(n.b);
            if (std::isnan(a) || std::isnan(b)) return kNaN;
            switch (n.op)
            {
                case Plus: return a + b;
                case Minus: return a - b;
                case Times: return a * b;
                case Divide: return a / b;
                case Power: return std::pow(a, b);
                case Less: return a < b ? 1.0 : 0.0;
                case LessEqual: return a <= b ? 1.0 : 0.0;
                case Greater: return a > b ? 1.0 : 0.0;
                case GreaterEqual: return a >= b ? 1.0 : 0.0;
                case Equal: return a == b ? 1.0 : 0.0;
                case NotEqual: return a != b ? 1.0 : 0.0;
                default: return kNaN;
            }
        }
        case Call:
        {
            const std::vector<int> &args = n.args;
            const double x = args.empty() ? kNaN : arg(args[0]);
            const double y = args.size() > 1 ? arg(args[1]) : kNaN;
            if (std::isnan(x)) return kNaN;
            switch (n.op)
            {
                case Sin: return std::sin(x);
                case Cos: return std::cos(x);
                case Tan: return std::tan(x);
                case Asin: return std::asin(x);
                case Acos: return std::acos(x);
                case Atan: return std::atan(x);
                case Atan2: return std::atan2(x, y);
                case Sinh: return std::sinh(x);
                case Cosh: return std::cosh(x);
                case Tanh: return std::tanh(x);
                case Sqrt: return std::sqrt(x);
                case Abs: return std::abs(x);
                case Exp: return std::exp(x);
                case Ln: return std::log(x);
                case Log10: return std::log10(x);
                case Log2: return std::log2(x);
                case Floor: return std::floor(x);
                case Ceil: return std::ceil(x);
                case Round: return std::round(x);
                case Sign: return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0;
                case Min:
                case Max:
                {
                    double v = x;
                    for (std::size_t k = 1; k < args.size(); ++k)
                    {
                        const double w = k == 1 ? y : arg(args[k]);
                        if (std::isnan(w)) return kNaN;
                        v = n.op == Min ? std::min(v, w) : std::max(v, w);
                    }
                    return v;
                }
                case Mod: return y != 0.0 ? x - y * std::floor(x / y) : kNaN;
                case Pow: return std::pow(x, y);
                case Sinc: return std::abs(x) < 1e-15 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                case Odd:
                case Even:
                {
                    if (!std::isfinite(x) || std::abs(x) > 9e15) return kNaN;
                    const bool odd = std::llround(x) % 2 != 0;
                    return (n.op == Odd) == odd ? 1.0 : 0.0;
                }
                case Sind: return std::sin(x * kPi / 180.0);
                case Cosd: return std::cos(x * kPi / 180.0);
                case Tand: return std::tan(x * kPi / 180.0);
                case Deg: return x * 180.0 / kPi;
                case Rad: return x * kPi / 180.0;
                default: return kNaN;
            }
        }
        default: return kNaN;
    }
}

Variables parseVariables(std::string_view text, std::span<const std::string> names, std::span<const double> values)
{
    Variables out;
    // Statements: separated by ";" or a new line out of any parentheses (in them, ";" separates arguments).
    std::vector<std::string_view> statements;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i)
    {
        const char c = i < text.size() ? text[i] : '\n';
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        else if ((c == ';' && depth == 0) || c == '\n')
        {
            statements.push_back(text.substr(start, i - start));
            start = i + 1;
            depth = 0;
        }
    }
    std::vector<std::string> known(names.begin(), names.end());
    std::vector<double> knownValues(values.begin(), values.end());
    knownValues.resize(known.size(), kNaN);
    for (std::string_view statement : statements)
    {
        statement = trim(statement);
        if (statement.empty()) continue;
        // The "=" of the assignment: not part of "==", "<=", ">=", "!=".
        std::size_t equal = std::string_view::npos;
        for (std::size_t i = 0; i < statement.size(); ++i)
        {
            if (statement[i] != '=') continue;
            const char before = i > 0 ? statement[i - 1] : ' ';
            const char after = i + 1 < statement.size() ? statement[i + 1] : ' ';
            if (before == '<' || before == '>' || before == '!' || before == '=' || after == '=') continue;
            equal = i;
            break;
        }
        const std::string shown(statement);
        if (equal == std::string_view::npos)
        {
            out.error = "Variables : « " + shown + " » : il faut « nom = valeur » (séparez les variables par « ; »).";
            return out;
        }
        const std::string name(trim(statement.substr(0, equal)));
        const std::string_view value = trim(statement.substr(equal + 1));
        if (!validName(name))
        {
            out.error = "Variables : « " + name + " » n'est pas un nom (une lettre d'abord, puis lettres, chiffres ou _).";
            return out;
        }
        if (isFormulaFunction(name) || name == "pi" || name == kPiSign || name == "e")
        {
            out.error = "Variables : « " + name + " » est déjà une fonction ou une constante : choisissez un autre nom.";
            return out;
        }
        if (std::find(names.begin(), names.end(), name) != names.end())
        {
            std::string listed;
            for (const std::string &reserved : names) listed += (listed.empty() ? "" : ", ") + reserved;
            out.error = "Variables : « " + name + " » est déjà un nom des formules (" + listed + ") : choisissez un autre nom.";
            return out;
        }
        if (std::find(out.names.begin(), out.names.end(), name) != out.names.end())
        {
            out.error = "Variables : « " + name + " » est défini deux fois.";
            return out;
        }
        if (value.empty())
        {
            out.error = "Variables : « " + name + " = » sans valeur.";
            return out;
        }
        std::string why;
        const Expression e = Expression::parse(value, known, &why);
        if (!e.valid())
        {
            out.error = "Variables, " + name + " : " + why;
            return out;
        }
        const double v = e.evaluate(knownValues);
        if (!std::isfinite(v))
        {
            out.error = "Variables, " + name + " : la valeur n'est pas un nombre fini.";
            return out;
        }
        out.names.push_back(name);
        out.values.push_back(v);
        known.push_back(name);
        knownValues.push_back(v);
    }
    return out;
}

bool isFormulaFunction(std::string_view name) noexcept
{
    return findFunction(name) != nullptr;
}
} // namespace qacq::analysis
