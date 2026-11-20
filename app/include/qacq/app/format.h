#pragma once

// Numbers as the interface shows them: French decimal comma, engineering prefixes ("48 kHz", "1,25 ms").

#include "qacq/model/signal.h"

#include <QString>

namespace qacq::app
{
// value in the base unit -> "1,25 ms", "48 kHz", "3,3 V"; "0 s" for zero; "—" for NaN.
QString formatQuantity(double value, const QString &unit, int significantDigits = 3);
QString formatCount(qint64 count);  // "480 000"
QString formatBytes(qint64 bytes);  // "1,5 Mo"
QString formatDuration(double seconds); // "12,5 s", "3 min 20 s", "1 h 05 min"
// "2 voies · 48 kHz · 10 s · 480 000 éch."
QString signalSummary(const model::Signal &signal);
} // namespace qacq::app
