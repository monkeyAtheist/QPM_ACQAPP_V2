#include "qacq/app/signal_list_model.h"
#include "qacq/analysis/calculus.h"

#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"

#include <QDateTime>
#include <QStringList>
#include <QVariantList>

#include <algorithm>

namespace qacq::app
{
SignalListModel::SignalListModel(QObject *parent) : QAbstractListModel(parent)
{
    m_document.setListener([this](const model::DocumentChange &change) { onDocumentChanged(change); });
}

int SignalListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant SignalListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const model::Signal &s = *m_rows[static_cast<std::size_t>(index.row())];
    switch (role)
    {
        case Qt::DisplayRole:
        case NameRole: return toQString(s.name);
        case IdRole: return QVariant::fromValue(static_cast<qint64>(s.id));
        case OriginRole: return toQString(std::string(model::originKey(s.origin)));
        case OriginNameRole: return toQString(std::string(model::originName(s.origin)));
        case RepresentationRole:
            return s.representation == model::Representation::IQ ? QStringLiteral("iq") : QStringLiteral("real");
        case ChannelCountRole: return static_cast<int>(s.channelCount());
        case ChannelNamesRole:
        {
            QStringList names;
            for (const auto &c : s.channels) names << toQString(c.name);
            return names;
        }
        case ChannelUnitsRole:
        {
            QStringList units;
            for (const auto &c : s.channels) units << toQString(c.unit);
            return units;
        }
        case FrameCountRole: return QVariant::fromValue(static_cast<qint64>(s.frameCount()));
        case SampleRateRole: return s.sampleRateHz;
        case DurationRole: return s.durationSeconds();
        case StartRole: return s.startSeconds;
        case NotesRole: return toQString(s.notes);
        case CreatedRole: return toQString(s.createdUtc);
        case DiscontinuityCountRole: return s.data ? static_cast<int>(s.data->discontinuities.size()) : 0;
        case BytesRole: return QVariant::fromValue(static_cast<qint64>(s.data ? s.data->byteSize() : 0));
        case SummaryRole: return signalSummary(s);
        default: return {};
    }
}

QHash<int, QByteArray> SignalListModel::roleNames() const
{
    return {
        {IdRole, "signalId"},
        {NameRole, "name"},
        {OriginRole, "origin"},
        {OriginNameRole, "originName"},
        {RepresentationRole, "representation"},
        {ChannelCountRole, "channelCount"},
        {ChannelNamesRole, "channelNames"},
        {ChannelUnitsRole, "channelUnits"},
        {FrameCountRole, "frameCount"},
        {SampleRateRole, "sampleRate"},
        {DurationRole, "duration"},
        {StartRole, "start"},
        {NotesRole, "notes"},
        {CreatedRole, "created"},
        {DiscontinuityCountRole, "discontinuityCount"},
        {BytesRole, "bytes"},
        {SummaryRole, "summary"},
    };
}

QString SignalListModel::undoText() const
{
    return toQString(m_document.undoText());
}

QString SignalListModel::redoText() const
{
    return toQString(m_document.redoText());
}

model::SignalPtr SignalListModel::signalAt(int row) const
{
    return row >= 0 && row < count() ? m_rows[static_cast<std::size_t>(row)] : nullptr;
}

model::SignalPtr SignalListModel::signalById(qint64 id) const
{
    return id > 0 ? m_document.find(static_cast<std::uint64_t>(id)) : nullptr;
}

qint64 SignalListModel::addSignal(model::Signal signal, const QString &label, QString *error)
{
    std::string why;
    const std::uint64_t id = m_document.add(std::move(signal), toUtf8(label), std::nullopt, &why);
    if (id == 0)
    {
        if (error) *error = toQString(why);
        fail(why);
    }
    return static_cast<qint64>(id);
}

bool SignalListModel::replaceSignal(qint64 id, model::Signal signal, const QString &label, quint64 mergeKey, QString *error)
{
    std::string why;
    const bool ok = id > 0 && m_document.replace(static_cast<std::uint64_t>(id), std::move(signal), toUtf8(label), mergeKey, &why);
    if (!ok)
    {
        if (why.empty()) why = "Ce signal n'existe plus.";
        if (error) *error = toQString(why);
        fail(why);
    }
    return ok;
}

void SignalListModel::resetSignals(std::vector<model::SignalPtr> signalList)
{
    m_document.reset(std::move(signalList));
    // reset() reports a change only if the list differs; the history and the modified flag changed anyway.
    refreshState();
}

void SignalListModel::markSaved()
{
    m_document.setClean();
    refreshState();
}

void SignalListModel::setHistoryLimits(qint64 bytes, int steps)
{
    m_document.setHistoryLimits(static_cast<std::size_t>(std::max<qint64>(0, bytes)), static_cast<std::size_t>(std::max(1, steps)));
    refreshState();
}

void SignalListModel::undo()
{
    m_document.undo();
}

void SignalListModel::redo()
{
    m_document.redo();
}

bool SignalListModel::removeSignal(qint64 id)
{
    std::string why;
    return id > 0 && m_document.remove(static_cast<std::uint64_t>(id), &why) ? true : fail(why);
}

bool SignalListModel::renameSignal(qint64 id, const QString &name)
{
    std::string why;
    return id > 0 && m_document.rename(static_cast<std::uint64_t>(id), toUtf8(name.trimmed()), &why) ? true : fail(why);
}

bool SignalListModel::moveSignal(int from, int to)
{
    if (from < 0 || to < 0) return false;
    return m_document.move(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
}

bool SignalListModel::setNotes(qint64 id, const QString &notes)
{
    const model::SignalPtr current = signalById(id);
    if (!current) return fail("Ce signal n'existe plus.");
    const std::string text = toUtf8(notes);
    if (text == current->notes) return true;
    model::Signal copy = *current;
    copy.notes = text;
    return replaceSignal(id, std::move(copy), QStringLiteral("Modifier les notes de « %1 »").arg(toQString(current->name)));
}

bool SignalListModel::setChannel(qint64 id, int channel, const QString &name, const QString &unit)
{
    const model::SignalPtr current = signalById(id);
    if (!current) return fail("Ce signal n'existe plus.");
    if (channel < 0 || channel >= static_cast<int>(current->channels.size())) return fail("Cette voie n'existe pas.");
    model::Signal copy = *current;
    auto &channelInfo = copy.channels[static_cast<std::size_t>(channel)];
    const std::string newName = toUtf8(name.trimmed()), newUnit = toUtf8(unit.trimmed());
    if (newName.empty()) return fail("Le nom d'une voie ne peut pas être vide.");
    if (channelInfo.name == newName && channelInfo.unit == newUnit) return true;
    channelInfo.name = newName;
    channelInfo.unit = newUnit;
    return replaceSignal(id, std::move(copy), QStringLiteral("Modifier la voie « %1 »").arg(name.trimmed()));
}

bool SignalListModel::setTiming(qint64 id, double sampleRateHz, double startSeconds)
{
    const model::SignalPtr current = signalById(id);
    if (!current) return fail("Ce signal n'existe plus.");
    if (current->sampleRateHz == sampleRateHz && current->startSeconds == startSeconds) return true;
    model::Signal copy = *current;
    copy.sampleRateHz = sampleRateHz;
    copy.startSeconds = startSeconds;
    // Same key for the same signal: holding a spin box makes a single step.
    const quint64 mergeKey = (static_cast<quint64>(id) << 8) | 0x01u;
    return replaceSignal(id, std::move(copy), QStringLiteral("Modifier l'échantillonnage de « %1 »").arg(toQString(current->name)),
                         mergeKey);
}

qint64 SignalListModel::deriveSignal(qint64 sourceId, const QVariantMap &options)
{
    const model::SignalPtr source = signalById(sourceId);
    if (!source)
    {
        fail("Ce signal n'existe plus.");
        return 0;
    }
    analysis::DeriveOptions o;
    if (!analysis::operationFromKey(toUtf8(options.value(QStringLiteral("operation")).toString()), o.operation))
    {
        fail("Calcul inconnu.");
        return 0;
    }
    const qint64 first = options.value(QStringLiteral("firstFrame"), 0).toLongLong();
    const qint64 count = options.value(QStringLiteral("frameCount"), -1).toLongLong();
    o.firstFrame = static_cast<std::size_t>(std::max<qint64>(0, first));
    if (count >= 0) o.frameCount = static_cast<std::size_t>(count);
    for (const QVariant &c : options.value(QStringLiteral("channels")).toList())
    {
        const int index = c.toInt();
        if (index >= 0) o.channels.push_back(static_cast<std::size_t>(index));
    }
    o.keepSource = options.value(QStringLiteral("keepSource"), true).toBool();
    o.removeMean = options.value(QStringLiteral("removeMean"), false).toBool();
    analysis::Derived derived = analysis::derive(*source, o);
    if (!derived.ok())
    {
        fail(derived.error);
        return 0;
    }
    model::Signal &signal = derived.signal;
    signal.name = m_document.uniqueName(signal.name);
    signal.createdUtc = toUtf8(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    const QString label = QStringLiteral("Calculer « %1 »").arg(toQString(signal.name));
    const std::optional<std::size_t> after = m_document.indexOf(source->id);
    std::string why;
    const std::uint64_t id = m_document.add(std::move(signal), toUtf8(label),
                                            after ? std::optional<std::size_t>(*after + 1) : std::nullopt, &why);
    if (id == 0) fail(why);
    return static_cast<qint64>(id);
}

void SignalListModel::endMerge()
{
    m_document.endMerge();
}

int SignalListModel::rowOf(qint64 id) const
{
    for (int row = 0; row < count(); ++row)
        if (static_cast<qint64>(m_rows[static_cast<std::size_t>(row)]->id) == id) return row;
    return -1;
}

qint64 SignalListModel::idAt(int row) const
{
    const model::SignalPtr s = signalAt(row);
    return s ? static_cast<qint64>(s->id) : 0;
}

QVariantMap SignalListModel::info(qint64 id) const
{
    const int row = rowOf(id);
    if (row < 0) return {};
    QVariantMap map;
    const QHash<int, QByteArray> roles = roleNames();
    const QModelIndex at = index(row);
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) map.insert(QString::fromLatin1(it.value()), data(at, it.key()));
    const model::Signal &s = *m_rows[static_cast<std::size_t>(row)];
    QVariantList channels;
    for (const auto &c : s.channels)
    {
        QVariantMap cm;
        cm.insert(QStringLiteral("name"), toQString(c.name));
        cm.insert(QStringLiteral("unit"), toQString(c.unit));
        channels.append(cm);
    }
    map.insert(QStringLiteral("channels"), channels);
    map.insert(QStringLiteral("provenance"), jsonToVariant(s.provenance));
    map.insert(QStringLiteral("sizeText"), formatBytes(static_cast<qint64>(s.data ? s.data->byteSize() : 0)));
    map.insert(QStringLiteral("rateText"), formatQuantity(s.sampleRateHz, QStringLiteral("Hz")));
    map.insert(QStringLiteral("durationText"), formatDuration(s.durationSeconds()));
    return map;
}

QString SignalListModel::uniqueName(const QString &base) const
{
    return toQString(m_document.uniqueName(toUtf8(base)));
}

void SignalListModel::onDocumentChanged(const model::DocumentChange &change)
{
    using Kind = model::DocumentChange::Kind;
    const std::vector<model::SignalPtr> &now = m_document.signalList();
    switch (change.kind)
    {
        case Kind::None: break;
        case Kind::Inserted:
        {
            const int row = static_cast<int>(change.first);
            beginInsertRows(QModelIndex(), row, row);
            m_rows = now;
            endInsertRows();
            break;
        }
        case Kind::Removed:
        {
            const int row = static_cast<int>(change.first);
            beginRemoveRows(QModelIndex(), row, row);
            m_rows = now;
            endRemoveRows();
            break;
        }
        case Kind::Moved:
        {
            const int from = static_cast<int>(change.first), to = static_cast<int>(change.second);
            // Qt counts the destination in the old list: one past the target when moving down.
            if (beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to))
            {
                m_rows = now;
                endMoveRows();
            }
            else
            {
                beginResetModel();
                m_rows = now;
                endResetModel();
            }
            break;
        }
        case Kind::Updated:
            m_rows = now;
            for (std::size_t row : change.rows)
            {
                const QModelIndex at = index(static_cast<int>(row));
                emit dataChanged(at, at);
            }
            break;
        case Kind::Reset:
            beginResetModel();
            m_rows = now;
            endResetModel();
            break;
    }
    refreshState();
}

void SignalListModel::refreshState()
{
    if (m_rows.size() != m_document.signalList().size() || m_rows != m_document.signalList())
    {
        // Defensive: never let the copy drift from the document.
        beginResetModel();
        m_rows = m_document.signalList();
        endResetModel();
    }
    if (count() != m_lastCount)
    {
        m_lastCount = count();
        emit countChanged();
    }
    emit historyChanged();
    if (isModified() != m_lastModified)
    {
        m_lastModified = isModified();
        emit modifiedChanged();
    }
}

bool SignalListModel::fail(const std::string &message)
{
    if (!message.empty()) emit errorOccurred(toQString(message));
    return false;
}
} // namespace qacq::app
