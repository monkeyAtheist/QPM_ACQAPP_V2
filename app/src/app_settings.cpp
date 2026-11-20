#include "qacq/app/app_settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace qacq::app
{
namespace
{
const QString kRecentFiles = QStringLiteral("files/recent");
const QString kLastDirectory = QStringLiteral("files/lastDirectory");

QString presetKey(const QString &category)
{
    return QStringLiteral("presets/") + category;
}

QString normalised(const QString &localFile)
{
    return QDir::cleanPath(QFileInfo(localFile).absoluteFilePath());
}
} // namespace

AppSettings::AppSettings(QObject *parent) : QObject(parent), m_settings(std::make_unique<QSettings>()) {}

AppSettings::AppSettings(const QString &iniFile, QObject *parent)
    : QObject(parent), m_settings(std::make_unique<QSettings>(iniFile, QSettings::IniFormat))
{
}

AppSettings::~AppSettings() = default;

QStringList AppSettings::recentFiles() const
{
    QStringList existing;
    const QStringList stored = m_settings->value(kRecentFiles).toStringList();
    for (const QString &file : stored)
        if (QFileInfo::exists(file)) existing << file;
    return existing;
}

void AppSettings::addRecentFile(const QString &localFile)
{
    if (localFile.isEmpty()) return;
    const QString file = normalised(localFile);
    QStringList list = m_settings->value(kRecentFiles).toStringList();
    list.removeAll(file);
    list.prepend(file);
    while (list.size() > kMaximumRecentFiles) list.removeLast();
    m_settings->setValue(kRecentFiles, list);
    setLastDirectory(QFileInfo(file).absolutePath());
    emit recentFilesChanged();
}

void AppSettings::removeRecentFile(const QString &localFile)
{
    QStringList list = m_settings->value(kRecentFiles).toStringList();
    if (list.removeAll(normalised(localFile)) > 0)
    {
        m_settings->setValue(kRecentFiles, list);
        emit recentFilesChanged();
    }
}

void AppSettings::clearRecentFiles()
{
    m_settings->remove(kRecentFiles);
    emit recentFilesChanged();
}

QString AppSettings::lastDirectory() const
{
    const QString dir = m_settings->value(kLastDirectory).toString();
    return !dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QDir::homePath();
}

void AppSettings::setLastDirectory(const QString &directory)
{
    if (directory.isEmpty() || m_settings->value(kLastDirectory).toString() == directory) return;
    m_settings->setValue(kLastDirectory, directory);
    emit lastDirectoryChanged();
}

QVariant AppSettings::value(const QString &key, const QVariant &fallback) const
{
    return m_settings->value(key, fallback);
}

void AppSettings::setValue(const QString &key, const QVariant &value)
{
    m_settings->setValue(key, value);
}

QVariantMap AppSettings::presets(const QString &category) const
{
    const QByteArray text = m_settings->value(presetKey(category)).toString().toUtf8();
    if (text.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(text);
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

void AppSettings::storePresets(const QString &category, const QVariantMap &all)
{
    if (all.isEmpty())
        m_settings->remove(presetKey(category));
    else
        m_settings->setValue(presetKey(category),
                             QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(all)).toJson(QJsonDocument::Compact)));
    emit presetsChanged(category);
}

QStringList AppSettings::presetNames(const QString &category) const
{
    return presets(category).keys();
}

QVariantMap AppSettings::preset(const QString &category, const QString &name) const
{
    return presets(category).value(name).toMap();
}

void AppSettings::savePreset(const QString &category, const QString &name, const QVariantMap &values)
{
    if (name.trimmed().isEmpty()) return;
    QVariantMap all = presets(category);
    all.insert(name.trimmed(), values);
    storePresets(category, all);
}

void AppSettings::removePreset(const QString &category, const QString &name)
{
    QVariantMap all = presets(category);
    if (all.remove(name) > 0) storePresets(category, all);
}

void AppSettings::sync()
{
    m_settings->sync();
}

QString AppSettings::fileName() const
{
    return m_settings->fileName();
}
} // namespace qacq::app
