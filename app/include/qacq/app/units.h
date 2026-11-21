#pragma once

// Quantities for QML, shown and typed as the plots show them (qacq::plot): "1,25 ms", "48,00 kHz";
// "2 kHz", "1,5 ms", "115 200" typed in a field. A QML singleton ("Units").

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace qacq::app
{
class Units : public QObject
{
    Q_OBJECT

public:
    explicit Units(QObject *parent = nullptr);

    // `significant` digits with the prefix of the value: "1,234 ms".
    Q_INVOKABLE QString format(double value, const QString &unit, int significant = 4) const;
    // Shortest form: "2 kHz", "1,5 ms", "500 Hz" (what a field shows).
    Q_INVOKABLE QString compact(double value, const QString &unit, int significant = 6) const;
    // {ok, value, error}: value in the unit itself; a bare number is read in bareFactor times the unit.
    Q_INVOKABLE QVariantMap parse(const QString &text, const QString &unit, double bareFactor = 1.0) const;
    // Factor of the prefix `value` is shown with (1000 for 48 kHz): a bare number typed over it keeps it.
    Q_INVOKABLE double shownFactor(double value, const QString &unit) const;
    Q_INVOKABLE QString count(qint64 value) const;   // "480 000"
    Q_INVOKABLE QString bytes(qint64 value) const;   // "1,5 Mo"
    Q_INVOKABLE QString duration(double seconds) const; // "3 min 20 s"
};
} // namespace qacq::app
