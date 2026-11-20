#pragma once

// Settings remembered from one run to the next (QSettings): recent files, last folder, last serial port
// and baud rate, and named presets per category (frame formats, instrument addresses...).
// Presets are stored as JSON text, readable in the settings file.

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <memory>

namespace qacq::app
{
class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(QString lastDirectory READ lastDirectory WRITE setLastDirectory NOTIFY lastDirectoryChanged)

public:
    static constexpr int kMaximumRecentFiles = 10;

    // The application's settings (organisation and application names of QCoreApplication).
    explicit AppSettings(QObject *parent = nullptr);
    // A given INI file (tests, portable installation).
    explicit AppSettings(const QString &iniFile, QObject *parent = nullptr);
    ~AppSettings() override;

    QStringList recentFiles() const; // most recent first, files that still exist
    Q_INVOKABLE void addRecentFile(const QString &localFile);
    Q_INVOKABLE void removeRecentFile(const QString &localFile);
    Q_INVOKABLE void clearRecentFiles();

    QString lastDirectory() const;
    void setLastDirectory(const QString &directory);

    Q_INVOKABLE QVariant value(const QString &key, const QVariant &fallback = QVariant()) const;
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);

    Q_INVOKABLE QStringList presetNames(const QString &category) const;
    Q_INVOKABLE QVariantMap preset(const QString &category, const QString &name) const;
    Q_INVOKABLE void savePreset(const QString &category, const QString &name, const QVariantMap &values);
    Q_INVOKABLE void removePreset(const QString &category, const QString &name);

    void sync();
    QString fileName() const;

signals:
    void recentFilesChanged();
    void lastDirectoryChanged();
    void presetsChanged(const QString &category);

private:
    QVariantMap presets(const QString &category) const;
    void storePresets(const QString &category, const QVariantMap &all);

    std::unique_ptr<QSettings> m_settings;
};
} // namespace qacq::app
