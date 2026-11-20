#pragma once

// Bridges between the Qt-free model (UTF-8 std::string, std::filesystem::path, qacq::model::Json) and Qt.

#include "qacq/model/json.h"

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <string>

namespace qacq::app
{
inline QString toQString(const std::string &utf8)
{
    return QString::fromStdString(utf8);
}

inline std::string toUtf8(const QString &text)
{
    return text.toStdString();
}

// Local file name -> path, without going through the 8-bit code page on Windows (accents in user names,
// OneDrive folders).
inline std::filesystem::path toPath(const QString &localFile)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(localFile.toStdWString());
#else
    return std::filesystem::path(localFile.toStdString());
#endif
}

inline QString fromPath(const std::filesystem::path &path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

// JSON <-> QVariant (null -> invalid QVariant, numbers -> double, arrays -> QVariantList, objects ->
// QVariantMap, whose keys come back sorted).
QVariant jsonToVariant(const model::Json &json);
model::Json variantToJson(const QVariant &value);
} // namespace qacq::app
