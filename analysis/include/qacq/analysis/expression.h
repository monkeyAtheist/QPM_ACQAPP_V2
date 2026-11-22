#pragma once

// Formulas of numbers typed in the interface (the coefficients of a Fourier series...), without Qt.
//
//   numbers      2   0.5 or 0,5 (decimal comma)   1e-3   2,5e3
//   operators    + − * / ^ (power, right to left; −2^2 = −4); also × · ÷ and the minus sign −;
//                a product may be implicit after a number, a name or a ")": 2n, 2pi, pi n, 2(n+1),
//                (n+1)(n−1) — but n(n+1) reads as a call of n, an error
//   comparisons  < <= > >= == != (also ≤ ≥ ≠): 1 when true, 0 otherwise
//   functions    sin cos tan asin acos atan atan2 sinh cosh tanh sqrt abs exp ln log (= log10) log10 log2
//                floor ceil round sign min max mod pow sinc (sin πx / πx) odd even sind cosd tand deg rad
//                if(condition; then; else): only the branch chosen is evaluated (if(n == 1; 0; 1/(n−1)))
//                arguments separated by ";" (or by "," when it is not a decimal comma: "min(n, 5)")
//   constants    pi (π), e; then the names the caller gives (case matters: E is not e).
//
// Errors are in French and say where: « Nom inconnu « E » (caractère 7) ».

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::analysis
{
class Expression
{
public:
    Expression() = default;

    // Parsed against `names` (their values are given at evaluation, in the same order). Invalid on failure,
    // with the reason in `error`. An empty text is invalid too ("Formule vide.").
    static Expression parse(std::string_view text, std::span<const std::string> names, std::string *error = nullptr);

    bool valid() const noexcept { return !m_nodes.empty(); }
    // NaN where it is undefined (division by zero, square root of a negative number...).
    double evaluate(std::span<const double> values) const noexcept;
    // Whether the formula uses the name of that index.
    bool uses(std::size_t name) const noexcept;

    struct Node
    {
        int kind = 0;
        int op = 0; // operator, function or index of a name
        double value = 0.0;
        int a = -1, b = -1, c = -1; // operands (indices of nodes)
        std::vector<int> args;      // functions of more arguments
    };

private:
    double eval(int node, std::span<const double> values, int depth) const noexcept;

    std::vector<Node> m_nodes;
    int m_root = -1;
};

// Variables typed as "E = 10 ; D = 0,25" (or one per line): each value may use `names` (with `values`) and
// the variables before it. A name is a letter followed by letters, digits or "_", not a function, a
// constant nor one of `names`.
struct Variables
{
    std::vector<std::string> names;
    std::vector<double> values;
    std::string error; // empty when all is well
};
Variables parseVariables(std::string_view text, std::span<const std::string> names, std::span<const double> values);

// Whether `name` is a function of the formulas ("sin", "if"...).
bool isFormulaFunction(std::string_view name) noexcept;
} // namespace qacq::analysis
