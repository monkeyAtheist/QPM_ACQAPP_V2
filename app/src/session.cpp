#include "qacq/app/session.h"

#include "qacq/app/qt_convert.h"
#include "qacq/model/csv.h"
#include "qacq/model/session_file.h"
#include "qacq/model/wav.h"

#include <QDateTime>
#include <QFileInfo>

#ifndef QACQ_VERSION
#define QACQ_VERSION "2.0.0"
#endif

namespace qacq::app
{
namespace
{
QString localFileOf(const QUrl &url)
{
    if (url.isLocalFile()) return url.toLocalFile();
    if (url.scheme().isEmpty()) return url.toString(); // a plain path
    return {};
}

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QStringList toQStringList(const std::vector<std::string> &list)
{
    QStringList out;
    for (const auto &s : list) out << toQString(s);
    return out;
}
} // namespace

QString Session::applicationName()
{
    return QStringLiteral("QTacquisitionApp ") + QStringLiteral(QACQ_VERSION);
}

Session::Session(AppSettings *settings, QObject *parent)
    : QObject(parent), m_settings(settings), m_signals(new SignalListModel(this))
{
    connect(m_signals, &SignalListModel::modifiedChanged, this, &Session::onModifiedChanged);
}

QString Session::fileName() const
{
    return m_fileUrl.isEmpty() ? QString() : QFileInfo(localFileOf(m_fileUrl)).fileName();
}

QString Session::title() const
{
    const QString name = m_fileUrl.isEmpty() ? QStringLiteral("Sans titre") : fileName();
    return name + (isModified() ? QStringLiteral(" •") : QString()) + QStringLiteral(" — QTacquisitionApp");
}

void Session::newSession()
{
    m_signals->resetSignals();
    m_moduleSettings = model::Json::object();
    setSettingsModified(false);
    setFile(QUrl());
    succeed();
    emit sessionReplaced();
}

bool Session::open(const QUrl &url)
{
    const QString local = localFileOf(url);
    if (local.isEmpty()) return fail(QStringLiteral("Seuls les fichiers locaux peuvent être ouverts."));
    std::string error;
    auto content = model::loadSession(toPath(local), &error);
    if (!content) return fail(toQString(error));
    m_signals->resetSignals(std::move(content->signalList));
    m_moduleSettings = content->settings.isObject() ? content->settings : model::Json::object();
    setSettingsModified(false);
    setFile(QUrl::fromLocalFile(local));
    if (m_settings) m_settings->addRecentFile(local);
    succeed();
    emit sessionReplaced();
    return true;
}

bool Session::save()
{
    if (m_fileUrl.isEmpty()) return false;
    return saveAs(m_fileUrl);
}

bool Session::saveAs(const QUrl &url)
{
    QString local = localFileOf(url);
    if (local.isEmpty()) return fail(QStringLiteral("Seuls les fichiers locaux peuvent être enregistrés."));
    if (QFileInfo(local).suffix().isEmpty()) local += QStringLiteral(".") + fileSuffix();
    if (!writeTo(local)) return false;
    m_signals->markSaved();
    setSettingsModified(false);
    setFile(QUrl::fromLocalFile(local));
    if (m_settings) m_settings->addRecentFile(local);
    succeed();
    return true;
}

bool Session::writeTo(const QString &localFile)
{
    model::SessionContent content;
    content.signalList = m_signals->document().signalList();
    content.settings = m_moduleSettings;
    content.application = toUtf8(applicationName());
    content.savedUtc = toUtf8(nowUtc());
    const std::string error = model::saveSession(toPath(localFile), content);
    if (!error.empty()) return fail(toQString(error));
    return true;
}

qint64 Session::importFile(const QUrl &url, const QVariantMap &options)
{
    const QString local = localFileOf(url);
    if (local.isEmpty())
    {
        fail(QStringLiteral("Seuls les fichiers locaux peuvent être importés."));
        return 0;
    }
    const QString suffix = QFileInfo(local).suffix().toLower();
    model::ImportResult result;
    if (suffix == QStringLiteral("wav"))
    {
        result = model::importWav(toPath(local));
    }
    else if (suffix == QStringLiteral("csv") || suffix == QStringLiteral("txt") || suffix == QStringLiteral("tsv") ||
             suffix == QStringLiteral("dat"))
    {
        model::CsvImportOptions csv;
        csv.decimalComma = options.value(QStringLiteral("decimalComma"), false).toBool();
        csv.firstColumnIsTime = options.value(QStringLiteral("timeColumn"), false).toBool();
        csv.sampleRateHz = options.value(QStringLiteral("sampleRateHz"), 1.0).toDouble();
        csv.unit = toUtf8(options.value(QStringLiteral("unit")).toString());
        result = model::importCsv(toPath(local), csv);
    }
    else
    {
        fail(QStringLiteral("Format non reconnu : « .%1 » (CSV, texte ou WAV attendu).").arg(suffix));
        return 0;
    }
    if (!result.ok())
    {
        fail(toQString(result.error));
        return 0;
    }
    model::Signal signal = std::move(result.signal);
    signal.name = toUtf8(m_signals->uniqueName(toQString(signal.name)));
    signal.createdUtc = toUtf8(nowUtc());
    const QString label = QStringLiteral("Importer « %1 »").arg(toQString(signal.name));
    QString error;
    const qint64 id = m_signals->addSignal(std::move(signal), label, &error);
    if (id == 0)
    {
        fail(error);
        return 0;
    }
    if (m_settings) m_settings->setLastDirectory(QFileInfo(local).absolutePath());
    succeed(toQStringList(result.warnings));
    return id;
}

bool Session::exportSignal(qint64 id, const QUrl &url, const QVariantMap &options)
{
    const model::SignalPtr signal = m_signals->signalById(id);
    if (!signal) return fail(QStringLiteral("Ce signal n'existe plus."));
    const QString local = localFileOf(url);
    if (local.isEmpty()) return fail(QStringLiteral("Seuls les fichiers locaux peuvent être écrits."));
    const QString suffix = QFileInfo(local).suffix().toLower();
    std::string error;
    std::vector<std::string> warnings;
    if (suffix == QStringLiteral("csv") || suffix == QStringLiteral("txt"))
    {
        model::CsvExportOptions csv;
        csv.decimalComma = options.value(QStringLiteral("decimalComma"), false).toBool();
        csv.timeColumn = options.value(QStringLiteral("timeColumn"), true).toBool();
        error = model::exportCsv(toPath(local), *signal, csv);
    }
    else if (suffix == QStringLiteral("wav"))
    {
        model::WavWriteOptions wav;
        const QString encoding = options.value(QStringLiteral("encoding"), QStringLiteral("pcm16")).toString();
        wav.encoding = encoding == QStringLiteral("pcm24")     ? model::WavEncoding::Pcm24
                       : encoding == QStringLiteral("pcm32")   ? model::WavEncoding::Pcm32
                       : encoding == QStringLiteral("float32") ? model::WavEncoding::Float32
                                                               : model::WavEncoding::Pcm16;
        wav.fullScale = options.value(QStringLiteral("fullScale"), 1.0).toDouble();
        wav.normalize = options.value(QStringLiteral("normalize"), false).toBool();
        error = model::exportWav(toPath(local), *signal, wav, &warnings);
    }
    else
    {
        return fail(QStringLiteral("Format d'export non reconnu : « .%1 » (CSV ou WAV).").arg(suffix));
    }
    if (!error.empty()) return fail(toQString(error));
    if (m_settings) m_settings->setLastDirectory(QFileInfo(local).absolutePath());
    succeed(toQStringList(warnings));
    return true;
}

QVariantMap Session::moduleSettings(const QString &module) const
{
    const model::Json &settings = m_moduleSettings;
    return jsonToVariant(settings[toUtf8(module)]).toMap();
}

void Session::setModuleSettings(const QString &module, const QVariantMap &values)
{
    const model::Json json = variantToJson(values);
    const model::Json &current = m_moduleSettings;
    if (current[toUtf8(module)] == json) return;
    m_moduleSettings[toUtf8(module)] = json;
    setSettingsModified(true);
}

QStringList Session::sessionFilters() const
{
    return {QStringLiteral("Sessions QTacquisitionApp (*.qacq)"), QStringLiteral("Tous les fichiers (*)")};
}

QStringList Session::importFilters() const
{
    return {QStringLiteral("Mesures (*.csv *.txt *.tsv *.dat *.wav)"), QStringLiteral("Texte, CSV (*.csv *.txt *.tsv *.dat)"),
            QStringLiteral("Son, WAV (*.wav)"), QStringLiteral("Tous les fichiers (*)")};
}

QStringList Session::exportFilters() const
{
    return {QStringLiteral("CSV (*.csv)"), QStringLiteral("WAV (*.wav)")};
}

bool Session::fail(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
    return false;
}

void Session::succeed(const QStringList &warnings)
{
    if (!m_lastError.isEmpty())
    {
        m_lastError.clear();
        emit lastErrorChanged();
    }
    if (m_lastWarnings != warnings)
    {
        m_lastWarnings = warnings;
        emit lastWarningsChanged();
    }
}

void Session::setFile(const QUrl &url)
{
    if (m_fileUrl == url) return;
    m_fileUrl = url;
    emit fileChanged();
    emit titleChanged();
}

void Session::setSettingsModified(bool modified)
{
    m_settingsModified = modified;
    onModifiedChanged();
}

void Session::onModifiedChanged()
{
    if (isModified() == m_lastModified) return;
    m_lastModified = isModified();
    emit modifiedChanged();
    emit titleChanged();
}
} // namespace qacq::app
