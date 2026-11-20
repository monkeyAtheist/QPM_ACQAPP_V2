#pragma once

// The session the interface works on: its signals (SignalListModel, with undo), the settings of the
// modules, and the file it lives in. New, open, save, save as; import and export of CSV and WAV files.
// Every failure leaves the session as it was and sets lastError (in French) for the interface.

#include "qacq/app/app_settings.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/model/json.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

namespace qacq::app
{
class Session : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qacq::app::SignalListModel *signalList READ signalList CONSTANT)
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QStringList lastWarnings READ lastWarnings NOTIFY lastWarningsChanged)

public:
    static QString applicationName(); // "QTacquisitionApp 2.0.0"
    static QString fileSuffix() { return QStringLiteral("qacq"); }

    // settings may be null (tests); otherwise recent files and the last folder are kept up to date.
    explicit Session(AppSettings *settings = nullptr, QObject *parent = nullptr);

    SignalListModel *signalList() const { return m_signals; }
    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;
    QString title() const;
    bool isModified() const { return m_signals->isModified() || m_settingsModified; }
    QString lastError() const { return m_lastError; }
    QStringList lastWarnings() const { return m_lastWarnings; }

    Q_INVOKABLE void newSession();
    Q_INVOKABLE bool open(const QUrl &url);
    // Saves to the current file; false when there is none yet (the interface then asks for one: saveAs).
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl &url);

    // Import by extension: .csv, .txt, .tsv, .dat (text) or .wav. Options: decimalComma (bool),
    // timeColumn (bool), sampleRateHz (number), unit (string). Returns the id of the new signal, 0 on failure.
    Q_INVOKABLE qint64 importFile(const QUrl &url, const QVariantMap &options = QVariantMap());
    // Export by extension: .csv (decimalComma, timeColumn) or .wav (encoding "pcm16" | "pcm24" | "pcm32" |
    // "float32", fullScale, normalize).
    Q_INVOKABLE bool exportSignal(qint64 id, const QUrl &url, const QVariantMap &options = QVariantMap());

    // Settings of a module ("filters", "generator"...), saved in the session file. Not part of the undo
    // history (each module keeps its own), but they mark the session as modified.
    Q_INVOKABLE QVariantMap moduleSettings(const QString &module) const;
    Q_INVOKABLE void setModuleSettings(const QString &module, const QVariantMap &values);

    // Name filters for the file dialogs.
    Q_INVOKABLE QStringList sessionFilters() const;
    Q_INVOKABLE QStringList importFilters() const;
    Q_INVOKABLE QStringList exportFilters() const;

signals:
    void fileChanged();
    void titleChanged();
    void modifiedChanged();
    void lastErrorChanged();
    void lastWarningsChanged();
    // The whole session was replaced (new or opened): modules reload their settings.
    void sessionReplaced();

private:
    bool fail(const QString &message);
    void succeed(const QStringList &warnings = QStringList());
    void setFile(const QUrl &url);
    void setSettingsModified(bool modified);
    bool writeTo(const QString &localFile);
    void onModifiedChanged();

    AppSettings *m_settings = nullptr;
    SignalListModel *m_signals = nullptr;
    model::Json m_moduleSettings = model::Json::object();
    bool m_settingsModified = false;
    bool m_lastModified = false;
    QUrl m_fileUrl;
    QString m_lastError;
    QStringList m_lastWarnings;
};
} // namespace qacq::app
