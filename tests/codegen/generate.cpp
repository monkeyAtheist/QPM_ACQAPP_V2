// Writes the C of a few filters in every number format into a directory, and codegen_main.c, which takes them
// all in (one translation unit: their names must not clash) and runs their self-tests. Built and run by CTest
// (test "codegen") with the C compiler of the build: what a board would compile.
#include "qacq/analysis/filter_code.h"
#include "qacq/analysis/filters.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace qacq;
using namespace qacq::analysis;

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: qacq_codegen <directory>\n");
        return 2;
    }
    const std::string dir = argv[1];
    struct Case
    {
        std::string name;
        FilterRecipe recipe;
    };
    std::vector<Case> cases;
    FilterRecipe lp = defaultFilterRecipe(48000.0);
    lp.order = 6;
    lp.frequency1Hz = 2000.0;
    cases.push_back({"pb", lp});
    FilterRecipe hp = lp;
    hp.response = dsp::FilterResponse::HighPass;
    hp.order = 4;
    hp.frequency1Hz = 500.0;
    hp.gainDb = 6.0;
    cases.push_back({"ph", hp});
    FilterRecipe notch = lp;
    notch.response = dsp::FilterResponse::BandStop;
    notch.order = 2;
    notch.sampleRateHz = 1000.0;
    notch.frequency1Hz = 45.0;
    notch.frequency2Hz = 55.0;
    cases.push_back({"cb", notch});
    FilterRecipe elliptic = lp;
    elliptic.family = dsp::FilterFamily::Elliptic;
    elliptic.order = 5;
    elliptic.frequency1Hz = 1000.0;
    elliptic.passbandRippleDb = 0.5;
    elliptic.stopbandAttenuationDb = 60.0;
    cases.push_back({"el", elliptic});
    FilterRecipe fir = lp;
    fir.kind = FilterKind::Fir;
    fir.taps = 101;
    cases.push_back({"rif", fir});
    FilterRecipe even = fir;
    even.taps = 100;
    even.gainDb = -3.0;
    cases.push_back({"rifp", even});
    FilterRecipe smoothing = lp;
    smoothing.kind = FilterKind::Typed;
    smoothing.numerator = "alpha";
    smoothing.denominator = "1 ; alpha - 1";
    smoothing.variables = "alpha = 0,1";
    cases.push_back({"lis", smoothing});
    FilterRecipe skew = lp;
    skew.kind = FilterKind::Typed;
    skew.numerator = "0,1 ; 0,2 ; 0,3 ; 0,2";
    skew.denominator = "1";
    cases.push_back({"ns", skew});
    FilterRecipe derivative = lp;
    derivative.kind = FilterKind::Typed;
    derivative.numerator = "fs ; -fs";
    derivative.denominator = "1";
    cases.push_back({"der", derivative});

    std::string includes, calls;
    int written = 0;
    for (const Case &c : cases)
    {
        const FilterDesign design = designFilter(c.recipe);
        for (CodeFormat format : {CodeFormat::Float32, CodeFormat::Float64, CodeFormat::Q15, CodeFormat::Q31})
            for (CodeScaling scaling : {CodeScaling::Sum, CodeScaling::Peak})
            {
                const bool fixed = format == CodeFormat::Q15 || format == CodeFormat::Q31;
                if (!fixed && scaling == CodeScaling::Peak) continue; // the same code
                CodeOptions options;
                options.format = format;
                options.scaling = scaling;
                options.name = c.name + "_" + std::string(codeFormatKey(format)) + (fixed ? "_" + std::string(codeScalingKey(scaling)) : "");
                const EmbeddedFilter e = embedFilter(design, options);
                if (!e.ok()) continue; // the derivative does not fit in Q15: said, and no code
                const GeneratedCode code = generateCode(e, design);
                std::ofstream(dir + "/" + code.headerName, std::ios::binary) << code.header;
                std::ofstream(dir + "/" + code.sourceName, std::ios::binary) << code.source;
                includes += "#include \"" + code.sourceName + "\"\n";
                calls += "    r = " + options.name + "_autotest();\n    if (r != 0)\n        printf(\"" + options.name +
                         " : %d écarts\\n\", r);\n    total += r;\n";
                ++written;
            }
    }
    std::string main = "/* Généré par qacq_codegen : l'autotest de chaque filtre écrit en C. */\n#include <stdio.h>\n\n" + includes +
                       "\nint main(void)\n{\n    int r, total = 0;\n" + calls + "    printf(\"" + std::to_string(written) +
                       " filtres, %d écarts\\n\", total);\n    return total != 0;\n}\n";
    std::ofstream(dir + "/codegen_main.c", std::ios::binary) << main;
    std::printf("%d filtres écrits dans %s\n", written, dir.c_str());
    return 0;
}
