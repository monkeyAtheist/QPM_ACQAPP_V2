#pragma once

// CSV import and export.
//
// Export: a header line ("temps (s),Voie 1 (V),..."), then one line per sample. Numbers are written with
// the shortest text that reads back exactly. With decimalComma (French spreadsheets) the columns are
// separated by ';' and the decimal separator is ','.
// Import: any file the text decoder reads (separators, decimal comma, comments); an optional header line
// gives the channel names and units ("Voie 1 (V)" or "Voie 1 [V]"); an optional first column of times in
// seconds gives the sample rate (median step) and the start time.

#include "qacq/model/signal.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace qacq::model
{
struct CsvExportOptions
{
    bool decimalComma = false;
    bool timeColumn = true;
    bool header = true;
};

std::string writeCsv(std::ostream &out, const Signal &signal, const CsvExportOptions &options = {});
std::string exportCsv(const std::filesystem::path &file, const Signal &signal, const CsvExportOptions &options = {});

struct CsvImportOptions
{
    bool decimalComma = false;
    bool firstColumnIsTime = false;
    double timeUnitSeconds = 1.0; // unit of the time column: 1e-3 for milliseconds
    double sampleRateHz = 1.0;    // without a time column
    std::string unit;             // for the channels the header gives no unit
};

// What a text file looks like, for the import dialog: decimal mark, header, time column.
// Decimal comma when numbers hold a comma and no point, with ';' or tabs between columns (French
// spreadsheets) or spaces. Time column when the first title says so ("Temps (s)", "t", "time [ms]") or,
// without header, when the first column grows by a regular step.
struct CsvSniff
{
    bool decimalComma = false;
    bool header = false;
    std::vector<std::string> titles;  // of the header, if any
    std::size_t columns = 0;          // numbers on a line (time column included)
    std::size_t lines = 0;            // lines of numbers among those read
    bool firstColumnIsTime = false;
    double timeUnitSeconds = 1.0;     // 1e-3 when the title says ms
    double timeStepSeconds = 0.0;     // median step of the time column, 0 without one
    std::vector<std::string> preview; // the first lines, as they are (at most 8)
};
CsvSniff sniffCsv(std::istream &in, std::size_t maximumLines = 200);
CsvSniff sniffCsv(const std::filesystem::path &file, std::size_t maximumLines = 200);

struct ImportResult
{
    Signal signal; // id 0, origin Imported
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

ImportResult readCsv(std::istream &in, const std::string &name, const CsvImportOptions &options = {});
ImportResult importCsv(const std::filesystem::path &file, const CsvImportOptions &options = {});

// "Voie 1 (V)" -> {"Voie 1", "V"}; "x" -> {"x", ""}.
ChannelInfo parseColumnTitle(const std::string &title);
} // namespace qacq::model
