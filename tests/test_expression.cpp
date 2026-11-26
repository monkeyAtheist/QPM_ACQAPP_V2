#include "qacq_test.h"

#include "qacq/analysis/expression.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

using namespace qacq::analysis;

namespace
{
// The value of a formula of n (and E), or NaN with the error in `error`.
double value(const std::string &text, double n = 3.0, double e = 2.0, std::string *error = nullptr)
{
    const std::vector<std::string> names = {"n", "E"};
    std::string why;
    const Expression x = Expression::parse(text, names, &why);
    if (error) *error = why;
    if (!x.valid()) return std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> values = {n, e};
    return x.evaluate(values);
}

std::string error(const std::string &text)
{
    std::string why;
    value(text, 3.0, 2.0, &why);
    return why;
}

bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("formulas: operators, precedence, implicit products, decimal comma", "[analysis]")
{
    const double pi = std::numbers::pi;
    CHECK(value("1 + 2 * 3") == 7.0);
    CHECK(value("(1 + 2) * 3") == 9.0);
    CHECK(value("2 ^ 3 ^ 2") == 512.0); // right to left
    CHECK(value("-2^2") == -4.0);
    CHECK(value("2^-1") == 0.5);
    CHECK(value("8 / 4 / 2") == 1.0);
    CHECK(value("10 - 4 - 3") == 3.0);
    CHECK(value("0,5 + .25 + 1.") == 1.75);
    CHECK(value("2,5e3") == 2500.0);
    CHECK(value("1e-3 * 4") == 0.004);
    CHECK_NEAR(value("4*E/(pi*n)"), 8.0 / (3.0 * pi), 1e-15, 0.0);
    CHECK_NEAR(value("4E/(π n)"), 8.0 / (3.0 * pi), 1e-15, 0.0) ; // implicit, π, and "4E" is 4·E
    CHECK(value("2n") == 6.0);
    CHECK(value("2(n+1)") == 8.0);
    CHECK(value("(n+1)(n-1)") == 8.0);
    CHECK(value("(n+1)n") == 12.0);
    CHECK(value("E n") == 6.0);
    CHECK_NEAR(value("2e"), 2.0 * std::exp(1.0), 1e-15, 0.0); // 2 times e, not an exponent
    CHECK(value("3 \xC3\x97 2 \xC2\xB7 2 \xC3\xB7 4 \xE2\x88\x92 1") == 2.0); // × · ÷ −
    CHECK(value("\xE2\x88\x92n") == -3.0);
    CHECK(contains(error("1\xC2\xA0" "000 + 1"), "Il manque un opérateur")); // a space inside a number: two numbers
}

TEST_CASE("formulas: comparisons, functions, if, the names given", "[analysis]")
{
    const double pi = std::numbers::pi;
    CHECK(value("n > 2") == 1.0);
    CHECK(value("n <= 2") == 0.0);
    CHECK(value("n == 3") == 1.0);
    CHECK(value("n = 3") == 1.0);
    CHECK(value("n != 3") == 0.0);
    CHECK(value("n \xE2\x89\xA4 3") == 1.0); // ≤
    CHECK(value("(n > 1) * (n < 5)") == 1.0);
    CHECK_NEAR(value("sin(n*pi/2)"), -1.0, 0.0, 1e-15);
    CHECK(value("odd(n)") == 1.0);
    CHECK(value("even(n)") == 0.0);
    CHECK(value("odd(4)") == 0.0);
    CHECK_NEAR(value("sinc(0,5)"), 2.0 / pi, 1e-15, 0.0);
    CHECK(value("sinc(0)") == 1.0);
    CHECK(value("mod(-1; 3)") == 2.0);
    CHECK(value("min(n; 5; 1)") == 1.0);
    CHECK(value("max(n, 5)") == 5.0); // "," then a space: a separator
    CHECK(value("pow(2; 10)") == 1024.0);
    CHECK_NEAR(value("atan2(1; 1)"), pi / 4.0, 1e-15, 0.0);
    CHECK_NEAR(value("sind(30)"), 0.5, 0.0, 1e-15);
    CHECK_NEAR(value("deg(pi)"), 180.0, 1e-15, 0.0);
    CHECK_NEAR(value("log(1000)"), 3.0, 1e-15, 0.0);
    CHECK_NEAR(value("ln(e)"), 1.0, 1e-15, 0.0);
    CHECK(value("abs(-n) + sign(-2) + floor(2,7) + ceil(2,1) + round(2,5)") == 3.0 - 1.0 + 2.0 + 3.0 + 3.0);
    // if: only the branch chosen is evaluated.
    CHECK(value("if(n == 1; 0; 1/(n-1))", 1.0) == 0.0);
    CHECK(value("if(n == 1; 0; 1/(n-1))", 3.0) == 0.5);
    // Undefined: NaN (or infinite), never an exception.
    CHECK(std::isinf(value("1/(n-3)")));
    CHECK(std::isnan(value("sqrt(-1)")));
    CHECK(std::isnan(value("0 * (1/(n-3))")));

    const std::vector<std::string> names = {"n", "N", "Nmax"};
    const Expression x = Expression::parse("n / Nmax", names);
    REQUIRE(x.valid());
    CHECK(x.uses(0) && !x.uses(1) && x.uses(2));
    const std::vector<double> values = {2.0, 2.0, 8.0};
    CHECK(x.evaluate(values) == 0.25);
}

TEST_CASE("formulas: errors say what and where, in French", "[analysis]")
{
    CHECK(error("") == "Formule vide. (caractère 1)");
    CHECK(contains(error("4*D/n"), "Nom inconnu « D »"));
    CHECK(contains(error("4*D/n"), "(caractère 3)"));
    CHECK(contains(error("sinx(n)"), "Fonction inconnue « sinx »"));
    CHECK(contains(error("sin n"), "« sin » est une fonction"));
    CHECK(contains(error("n(n+1)"), "« n » n'est pas une fonction : un « * » manque-t-il ?"));
    CHECK(contains(error("(n+1"), "Il manque une parenthèse fermante"));
    CHECK(contains(error("n+1)"), "Parenthèse fermante « ) » de trop"));
    CHECK(contains(error("n 2"), "Il manque un opérateur avant « 2 »"));
    CHECK(contains(error("n +"), "une valeur manque à la fin"));
    CHECK(contains(error("2**3"), "La puissance s'écrit « ^ »"));
    CHECK(contains(error("pow(2,5)"), "séparez-les par « ; »"));
    CHECK(contains(error("1 < n < 5"), "Une seule comparaison à la fois"));
    CHECK(contains(error("t * n"), "« t » n'existe pas ici"));
    CHECK(contains(error("n ? 2"), "Il manque un opérateur avant « ? 2 »"));
    CHECK(contains(error("\xC3\xA9t\xC3\xA9"), "Nom inconnu « \xC3\xA9t\xC3\xA9 »")); // été: a name
    CHECK(contains(error("n + \xE2\x82\xAC"), "Nom inconnu")); // € is not an operator
    CHECK(contains(error(std::string(5000, '1')), "trop longue"));
    CHECK(contains(error(std::string(300, '(') + "1" + std::string(300, ')')), "trop imbriquée"));
}

TEST_CASE("formulas: variables, each one from those before it", "[analysis]")
{
    const std::vector<std::string> names = {"n", "f0"};
    const std::vector<double> values = {0.0, 50.0};
    const Variables v = parseVariables("E = 10 ; D = 0,25\ntheta = pi/4\nw = 2*pi*f0 ; P = pow(E; 2) / 2", names, values);
    REQUIRE(v.error.empty());
    REQUIRE(v.names.size() == 5);
    CHECK(v.names[0] == "E" && v.values[0] == 10.0);
    CHECK(v.values[1] == 0.25);
    CHECK_NEAR(v.values[2], std::numbers::pi / 4.0, 1e-15, 0.0);
    CHECK_NEAR(v.values[3], 100.0 * std::numbers::pi, 1e-15, 0.0);
    CHECK(v.values[4] == 50.0);
    CHECK(parseVariables("", names, values).names.empty());
    CHECK(parseVariables(" ; \n ", names, values).error.empty());

    CHECK(contains(parseVariables("E 10", names, values).error, "il faut « nom = valeur »"));
    CHECK(contains(parseVariables("2E = 1", names, values).error, "n'est pas un nom"));
    CHECK(contains(parseVariables("sin = 1", names, values).error, "déjà une fonction ou une constante"));
    CHECK(contains(parseVariables("n = 1", names, values).error, "« n » est déjà un nom des formules (n"));
    CHECK(contains(parseVariables("E = 1; E = 2", names, values).error, "défini deux fois"));
    CHECK(contains(parseVariables("E =", names, values).error, "sans valeur"));
    CHECK(contains(parseVariables("E = F + 1", names, values).error, "Variables, E : Nom inconnu « F »"));
    CHECK(contains(parseVariables("E = 1/0", names, values).error, "pas un nombre fini"));
    CHECK(contains(parseVariables("E = 10, D = 2", names, values).error, "hors d'une fonction"));
    CHECK(isFormulaFunction("if") && isFormulaFunction("sinc") && !isFormulaFunction("E"));
}
