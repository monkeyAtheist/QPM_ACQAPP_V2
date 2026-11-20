#include "qacq/app/qt_convert.h"

#include <QMetaType>

namespace qacq::app
{
QVariant jsonToVariant(const model::Json &json)
{
    switch (json.type())
    {
        case model::Json::Type::Null: return {};
        case model::Json::Type::Bool: return QVariant(json.toBool());
        case model::Json::Type::Number: return QVariant(json.toNumber());
        case model::Json::Type::String: return QVariant(toQString(json.toString()));
        case model::Json::Type::Array:
        {
            QVariantList list;
            for (const model::Json &item : json.items()) list.append(jsonToVariant(item));
            return list;
        }
        case model::Json::Type::Object:
        {
            QVariantMap map;
            for (const model::JsonMember &member : json.members()) map.insert(toQString(member.key), jsonToVariant(member.value));
            return map;
        }
    }
    return {};
}

model::Json variantToJson(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) return {};
    switch (value.metaType().id())
    {
        case QMetaType::Bool: return model::Json(value.toBool());
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::Long:
        case QMetaType::ULong:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Short:
        case QMetaType::UShort:
        case QMetaType::Double:
        case QMetaType::Float: return model::Json(value.toDouble());
        case QMetaType::QString: return model::Json(toUtf8(value.toString()));
        case QMetaType::QStringList:
        {
            model::Json list = model::Json::array();
            for (const QString &s : value.toStringList()) list.push_back(model::Json(toUtf8(s)));
            return list;
        }
        case QMetaType::QVariantList:
        {
            model::Json list = model::Json::array();
            for (const QVariant &item : value.toList()) list.push_back(variantToJson(item));
            return list;
        }
        case QMetaType::QVariantMap:
        {
            model::Json object = model::Json::object();
            const QVariantMap map = value.toMap();
            for (auto it = map.cbegin(); it != map.cend(); ++it) object[toUtf8(it.key())] = variantToJson(it.value());
            return object;
        }
        default:
            // QUrl, QDateTime, JS values converted by the engine...: their text form.
            if (value.canConvert<QString>()) return model::Json(toUtf8(value.toString()));
            return {};
    }
}
} // namespace qacq::app
