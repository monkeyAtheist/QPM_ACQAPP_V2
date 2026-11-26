#pragma once

// The signals of the session as a list model for QML, with undo and redo: a thin Qt layer over
// qacq::model::Document (which holds the data and the history, and is tested without Qt).
//
// The model keeps its own copy of the list of pointers and updates it between begin...Rows() and
// end...Rows(), so views always see a consistent count and contents.

#include "qacq/model/document.h"

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

namespace qacq::app
{
class SignalListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged)
    Q_PROPERTY(qint64 historyBytes READ historyBytes NOTIFY historyChanged)

public:
    enum Role
    {
        IdRole = Qt::UserRole + 1,
        NameRole,
        OriginRole,      // "acquired", "generated", "imported", "derived"
        OriginNameRole,  // "Acquis"...
        RepresentationRole, // "real" or "iq"
        ChannelCountRole,
        ChannelNamesRole, // QStringList
        ChannelUnitsRole, // QStringList
        FrameCountRole,
        SampleRateRole,
        DurationRole,
        StartRole,
        NotesRole,
        CreatedRole,
        DiscontinuityCountRole,
        BytesRole,
        SummaryRole // "2 voies · 48 kHz · 10 s · 480 000 éch."
    };
    Q_ENUM(Role)

    explicit SignalListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_rows.size()); }
    bool canUndo() const { return m_document.canUndo(); }
    bool canRedo() const { return m_document.canRedo(); }
    QString undoText() const;
    QString redoText() const;
    bool isModified() const { return m_document.isModified(); }
    qint64 historyBytes() const { return static_cast<qint64>(m_document.historyBytes()); }

    // C++ side (modules, session).
    const model::Document &document() const { return m_document; }
    model::SignalPtr signalAt(int row) const;
    model::SignalPtr signalById(qint64 id) const;
    qint64 addSignal(model::Signal signal, const QString &label, QString *error = nullptr);
    bool replaceSignal(qint64 id, model::Signal signal, const QString &label, quint64 mergeKey = 0,
                       QString *error = nullptr);
    void resetSignals(std::vector<model::SignalPtr> signalList = {});
    void markSaved();
    void setHistoryLimits(qint64 bytes, int steps);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE bool removeSignal(qint64 id);
    Q_INVOKABLE bool renameSignal(qint64 id, const QString &name);
    Q_INVOKABLE bool moveSignal(int from, int to);
    Q_INVOKABLE bool setNotes(qint64 id, const QString &notes);
    Q_INVOKABLE bool setChannel(qint64 id, int channel, const QString &name, const QString &unit);
    // A channel taken out of a signal, or only some of its channels kept (their indices, in the order they
    // will have): one step of the history each. A signal keeps at least one channel.
    Q_INVOKABLE bool removeChannel(qint64 id, int channel);
    Q_INVOKABLE bool keepChannels(qint64 id, const QVariantList &channels);
    // Dragged values (a spin box held down) merge into one undo step until endMerge().
    Q_INVOKABLE bool setTiming(qint64 id, double sampleRateHz, double startSeconds);
    // A curve computed from a signal (qacq/analysis/calculus.h), added just after it as one step of the
    // history. Options: operation ("integral", "derivative", "cycleMean", "cycleRms", "cycleFrequency"),
    // firstFrame, frameCount (-1: up to the end), channels (indices; empty: all), keepSource (bool, the
    // source channels first), removeMean (bool, integral). Returns the id of the new signal, 0 on failure
    // (errorOccurred says why).
    Q_INVOKABLE qint64 deriveSignal(qint64 sourceId, const QVariantMap &options);
    Q_INVOKABLE void endMerge();
    Q_INVOKABLE int rowOf(qint64 id) const;
    Q_INVOKABLE qint64 idAt(int row) const;
    // Everything about one signal, for the inspector: the roles plus channels [{name, unit}], provenance.
    Q_INVOKABLE QVariantMap info(qint64 id) const;
    Q_INVOKABLE QString uniqueName(const QString &base) const;

signals:
    void countChanged();
    void historyChanged();
    void modifiedChanged();
    void errorOccurred(const QString &message);

private:
    void onDocumentChanged(const model::DocumentChange &change);
    void refreshState();
    bool fail(const std::string &message);

    model::Document m_document;
    std::vector<model::SignalPtr> m_rows;
    int m_lastCount = 0;
    bool m_lastModified = false;
};
} // namespace qacq::app
