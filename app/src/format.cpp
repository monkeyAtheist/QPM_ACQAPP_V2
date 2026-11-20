#include "qacq/app/format.h"

#include <QLocale>
#include <QStringList>

#include <cmath>

namespace qacq::app
{
namespace
{
const QLocale &french()
{
    static const QLocale locale(QLocale::French, QLocale::France);
    return locale;
}
} // namespace

QString formatQuantity(double value, const QString &unit, int significantDigits)
{
    if (std::isnan(value)) return QStringLiteral("—");
    if (std::isinf(value)) return value > 0 ? QStringLiteral("∞") : QStringLiteral("−∞");
    const QString space = unit.isEmpty() ? QString() : QStringLiteral(" ");
    if (value == 0.0) return QStringLiteral("0") + space + unit;
    static const struct
    {
        double factor;
        const char *prefix;
    } prefixes[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1.0, ""}, {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"}, {1e-12, "p"}};
    const double magnitude = std::abs(value);
    double factor = 1.0;
    QString prefix;
    // Bare numbers and units that never take a prefix keep their value as is.
    const bool prefixed = !unit.isEmpty() && unit != QStringLiteral("%") && unit != QStringLiteral("dB") &&
                          unit != QStringLiteral("°") && unit != QStringLiteral("°C");
    if (prefixed)
    {
        for (const auto &p : prefixes)
        {
            if (magnitude >= p.factor * 0.9995)
            {
                factor = p.factor;
                prefix = QString::fromUtf8(p.prefix);
                break;
            }
        }
        if (magnitude < 1e-12)
        {
            factor = 1e-12;
            prefix = QStringLiteral("p");
        }
    }
    const QString number = french().toString(value / factor, 'g', significantDigits);
    return number + space + prefix + unit;
}

QString formatCount(qint64 count)
{
    return french().toString(count);
}

QString formatBytes(qint64 bytes)
{
    const double b = static_cast<double>(bytes);
    if (b < 1024.0) return french().toString(bytes) + QStringLiteral(" o");
    if (b < 1024.0 * 1024.0) return french().toString(b / 1024.0, 'f', 1) + QStringLiteral(" Kio");
    if (b < 1024.0 * 1024.0 * 1024.0) return french().toString(b / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" Mio");
    return french().toString(b / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QStringLiteral(" Gio");
}

QString formatDuration(double seconds)
{
    if (!std::isfinite(seconds)) return QStringLiteral("—");
    if (seconds < 60.0) return formatQuantity(seconds, QStringLiteral("s"));
    const auto total = static_cast<qint64>(std::llround(seconds));
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0) return QStringLiteral("%1 h %2 min").arg(h).arg(m, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1 min %2 s").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString signalSummary(const model::Signal &signal)
{
    QStringList parts;
    const auto channels = static_cast<qint64>(signal.channelCount());
    if (signal.representation == model::Representation::IQ)
        parts << (channels == 2 ? QStringLiteral("I/Q") : QStringLiteral("%1 paires I/Q").arg(channels / 2));
    else
        parts << (channels == 1 ? QStringLiteral("1 voie") : QStringLiteral("%1 voies").arg(channels));
    parts << formatQuantity(signal.sampleRateHz, QStringLiteral("Hz"));
    parts << formatDuration(signal.durationSeconds());
    parts << formatCount(static_cast<qint64>(signal.frameCount())) + QStringLiteral(" éch.");
    return parts.join(QStringLiteral(" · "));
}
} // namespace qacq::app
