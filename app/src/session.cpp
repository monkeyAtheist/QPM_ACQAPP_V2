#include "qacq/app/session.h"

#include "qacq/app/qt_convert.h"
#include "qacq/model/csv.h"
#include "qacq/model/session_file.h"
#include "qacq/model/wav.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>

#include <cmath>
#include <optional>

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

// What to read, in plain C++ (made here, read on the thread of imports): the options not given are the
// ones recognised in the file.
struct ImportRequest
{
    enum class Kind
    {
        Wav,
        Text,
        Unknown
    };
    std::filesystem::path path;
    Kind kind = Kind::Unknown;
    std::string suffix;
    std::optional<bool> decimalComma, timeColumn;
    std::optional<double> timeUnitSeconds;
    double sampleRateHz = 1.0;
    std::string unit;
};

ImportRequest importRequest(const QString &local, const QVariantMap &options)
{
    ImportRequest r;
    r.path = toPath(local);
    const QString suffix = QFileInfo(local).suffix().toLower();
    r.suffix = toUtf8(suffix);
    if (suffix == QStringLiteral("wav")) r.kind = ImportRequest::Kind::Wav;
    else if (suffix == QStringLiteral("csv") || suffix == QStringLiteral("txt") || suffix == QStringLiteral("tsv") ||
             suffix == QStringLiteral("dat"))
        r.kind = ImportRequest::Kind::Text;
    if (options.contains(QStringLiteral("decimalComma"))) r.decimalComma = options.value(QStringLiteral("decimalComma")).toBool();
    if (options.contains(QStringLiteral("timeColumn"))) r.timeColumn = options.value(QStringLiteral("timeColumn")).toBool();
    if (options.contains(QStringLiteral("timeUnitSeconds")))
        r.timeUnitSeconds = options.value(QStringLiteral("timeUnitSeconds")).toDouble();
    r.sampleRateHz = options.value(QStringLiteral("sampleRateHz"), 1.0).toDouble();
    r.unit = toUtf8(options.value(QStringLiteral("unit")).toString());
    return r;
}

model::ImportResult readImport(const ImportRequest &request, const model::Progress &progress)
{
    if (request.kind == ImportRequest::Kind::Wav) return model::importWav(request.path, progress);
    if (request.kind == ImportRequest::Kind::Text)
    {
        model::CsvImportOptions csv;
        if (!request.decimalComma || !request.timeColumn || !request.timeUnitSeconds)
        {
            const model::CsvSniff sniff = model::sniffCsv(request.path);
            csv.decimalComma = request.decimalComma.value_or(sniff.decimalComma);
            csv.firstColumnIsTime = request.timeColumn.value_or(sniff.firstColumnIsTime);
            csv.timeUnitSeconds = request.timeUnitSeconds.value_or(sniff.timeUnitSeconds);
        }
        else
        {
            csv.decimalComma = *request.decimalComma;
            csv.firstColumnIsTime = *request.timeColumn;
            csv.timeUnitSeconds = *request.timeUnitSeconds;
        }
        csv.sampleRateHz = request.sampleRateHz;
        csv.unit = request.unit;
        return model::importCsv(request.path, csv, progress);
    }
    model::ImportResult unknown;
    unknown.error = "Format non reconnu : « ." + request.suffix + " » (CSV, texte ou WAV attendu).";
    return unknown;
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
    m_importPool.setMaxThreadCount(1);
    m_importTimer.setInterval(100);
    connect(&m_importTimer, &QTimer::timeout, this, [this] {
        const std::shared_ptr<ImportRun> run = m_importRun;
        if (!run) return;
        const double done = 0.001 * static_cast<double>(run->permille.load());
        if (std::abs(done - m_importProgress) < 0.005) return;
        m_importProgress = done;
        emit importProgressChanged();
    });
}

Session::~Session()
{
    cancelImport();
    m_importPool.waitForDone();
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
    cancelImport(); // what it reads was for the session before
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
    cancelImport(); // what it reads was for the session before
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

QVariantMap Session::inspectImport(const QUrl &url) const
{
    QVariantMap m;
    const QString local = localFileOf(url);
    m.insert(QStringLiteral("fileName"), QFileInfo(local).fileName());
    const QString suffix = QFileInfo(local).suffix().toLower();
    if (local.isEmpty() || !QFileInfo::exists(local))
    {
        m.insert(QStringLiteral("ok"), false);
        m.insert(QStringLiteral("error"), QStringLiteral("Fichier introuvable."));
        return m;
    }
    if (suffix == QStringLiteral("wav"))
    {
        m.insert(QStringLiteral("ok"), true);
        m.insert(QStringLiteral("kind"), QStringLiteral("wav"));
        return m;
    }
    if (suffix != QStringLiteral("csv") && suffix != QStringLiteral("txt") && suffix != QStringLiteral("tsv") &&
        suffix != QStringLiteral("dat"))
    {
        m.insert(QStringLiteral("ok"), false);
        m.insert(QStringLiteral("error"), QStringLiteral("Format non reconnu : « .%1 » (CSV, texte ou WAV attendu).").arg(suffix));
        return m;
    }
    const model::CsvSniff sniff = model::sniffCsv(toPath(local));
    QStringList titles, preview;
    for (const std::string &t : sniff.titles) titles << toQString(t);
    for (const std::string &p : sniff.preview) preview << toQString(p);
    m.insert(QStringLiteral("ok"), sniff.lines > 0);
    if (sniff.lines == 0) m.insert(QStringLiteral("error"), QStringLiteral("Aucune ligne de nombres au début de ce fichier."));
    m.insert(QStringLiteral("kind"), QStringLiteral("csv"));
    m.insert(QStringLiteral("decimalComma"), sniff.decimalComma);
    m.insert(QStringLiteral("timeColumn"), sniff.firstColumnIsTime);
    m.insert(QStringLiteral("timeUnitSeconds"), sniff.timeUnitSeconds);
    m.insert(QStringLiteral("timeStepSeconds"), sniff.timeStepSeconds);
    m.insert(QStringLiteral("header"), sniff.header);
    m.insert(QStringLiteral("titles"), titles);
    m.insert(QStringLiteral("columns"), static_cast<int>(sniff.columns));
    m.insert(QStringLiteral("lines"), static_cast<int>(sniff.lines));
    m.insert(QStringLiteral("preview"), preview);
    return m;
}

qint64 Session::importFile(const QUrl &url, const QVariantMap &options)
{
    const QString local = localFileOf(url);
    if (local.isEmpty())
    {
        fail(QStringLiteral("Seuls les fichiers locaux peuvent être importés."));
        return 0;
    }
    model::ImportResult result = readImport(importRequest(local, options), {});
    return addImported(local, result);
}

bool Session::startImport(const QUrl &url, const QVariantMap &options)
{
    if (m_importing) return fail(QStringLiteral("Un import est déjà en cours : attendez qu'il finisse, ou annulez-le."));
    const QString local = localFileOf(url);
    if (local.isEmpty()) return fail(QStringLiteral("Seuls les fichiers locaux peuvent être importés."));
    const ImportRequest request = importRequest(local, options);
    auto run = std::make_shared<ImportRun>();
    m_importRun = run;
    m_importing = true;
    m_importName = QFileInfo(local).fileName();
    m_importProgress = 0.0;
    emit importingChanged();
    emit importProgressChanged();
    m_importTimer.start();
    m_importPool.start([this, local, request, run] {
        auto result = std::make_shared<model::ImportResult>(readImport(request, model::Progress{&run->cancelled, &run->permille}));
        // The destructor waits for this task: `this` is alive here, and the queued call is dropped if it is
        // destroyed before the call runs.
        QMetaObject::invokeMethod(this, [this, local, run, result] { importDone(local, run, result); }, Qt::QueuedConnection);
    });
    return true;
}

void Session::cancelImport()
{
    if (m_importRun) m_importRun->cancelled.store(true);
}

void Session::waitForImport()
{
    while (m_importing)
    {
        m_importPool.waitForDone();
        QCoreApplication::processEvents();
    }
}

void Session::importDone(const QString &localFile, const std::shared_ptr<ImportRun> &run,
                         const std::shared_ptr<model::ImportResult> &result)
{
    if (m_importRun == run)
    {
        m_importRun.reset();
        m_importTimer.stop();
    }
    const bool cancelled = run->cancelled.load() || result->cancelled;
    const qint64 id = cancelled ? 0 : addImported(localFile, *result);
    m_importing = false;
    m_importName.clear();
    emit importingChanged();
    emit importFinished(id, cancelled);
}

qint64 Session::addImported(const QString &localFile, model::ImportResult &result)
{
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
    if (m_settings) m_settings->setLastDirectory(QFileInfo(localFile).absolutePath());
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
