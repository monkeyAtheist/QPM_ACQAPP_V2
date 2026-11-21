#include "image_tools.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>

bool ImageTools::copy(const QVariant &image) const
{
    const QImage picture = image.value<QImage>();
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (picture.isNull() || !clipboard) return false;
    clipboard->setImage(picture);
    return true;
}
