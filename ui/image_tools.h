#pragma once
// Images of the plots for QML: the picture taken by Item.grabToImage() put on the clipboard (to paste in
// a report). Saving to a file is done by the grab result itself (saveToFile). Also a text to the clipboard
// (the coefficients of a filter).
#include <QObject>
#include <QVariant>

class ImageTools : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    // `image` is ItemGrabResult.image (a QImage in a QVariant). False when it holds no image.
    Q_INVOKABLE bool copy(const QVariant &image) const;
    // False when there is no clipboard (or nothing to copy).
    Q_INVOKABLE bool copyText(const QString &text) const;
};
