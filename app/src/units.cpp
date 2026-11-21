#include "qacq/app/units.h"

#include "qacq/app/format.h"
#include "qacq/plot/axis.h"

#include <cmath>
#include <string>

namespace qacq::app
{
namespace
{
std::string utf8(const QString &text)
{
    return text.toStdString();
}
} // namespace

Units::Units(QObject *parent) : QObject(parent) {}

QString Units::format(double value, const QString &unit, int significant) const
{
    return QString::fromStdString(plot::formatQuantity(value, utf8(unit), significant));
}

QString Units::compact(double value, const QString &unit, int significant) const
{
    return QString::fromStdString(plot::formatCompact(value, utf8(unit), significant));
}

QVariantMap Units::parse(const QString &text, const QString &unit, double bareFactor) const
{
    const plot::Parsed parsed = plot::parseQuantity(utf8(text), utf8(unit), bareFactor);
    QVariantMap out;
    out.insert(QStringLiteral("ok"), parsed.ok);
    out.insert(QStringLiteral("value"), parsed.value);
    out.insert(QStringLiteral("error"), QString::fromStdString(parsed.error));
    return out;
}

double Units::shownFactor(double value, const QString &unit) const
{
    const std::string u = utf8(unit);
    if (!plot::takesPrefix(u) || !(std::abs(value) > 0.0)) return 1.0;
    return plot::engineeringPrefix(std::abs(value) * 1.0000001, u == "s" ? 0 : 4).factor;
}

QString Units::count(qint64 value) const
{
    return formatCount(value);
}

QString Units::bytes(qint64 value) const
{
    return formatBytes(value);
}

QString Units::duration(double seconds) const
{
    return formatDuration(seconds);
}
} // namespace qacq::app
