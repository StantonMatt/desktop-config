// Pure capture checks shared by quick-region-overlay.cpp and tests/capture-checks-test.cpp.
#pragma once
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>

namespace CaptureChecks {

// Why a ScreenShot2 raw reply is unusable, or nullptr: not raw ("type"), an empty size ("size"), an unknown format
// ("format"), or a stride too small for width pixels of that format ("stride"; QImage would read past the mapping).
inline const char *rawBufferProblem(const QVariantMap &meta) {
    const int width = meta.value(QStringLiteral("width")).toInt();
    const int height = meta.value(QStringLiteral("height")).toInt();
    const int stride = meta.value(QStringLiteral("stride")).toInt();
    const uint format = meta.value(QStringLiteral("format")).toUInt();
    if (meta.value(QStringLiteral("type")).toString() != QLatin1String("raw")) return "type";
    if (width <= 0 || height <= 0) return "size";
    if (format <= uint(QImage::Format_Invalid) || format >= uint(QImage::NImageFormats)) return "format";
    const qint64 minStride = (qint64(width) * QImage::toPixelFormat(QImage::Format(format)).bitsPerPixel() + 7) / 8;
    if (minStride <= 0) return "format";
    if (stride < minStride) return "stride";
    return nullptr;
}

// Bytes a ScreenShot2 raw reply needs (stride x height), or 0 when rawBufferProblem rejects it.
inline qint64 rawBufferSize(const QVariantMap &meta) {
    if (rawBufferProblem(meta)) return 0;
    return qint64(meta.value(QStringLiteral("stride")).toInt()) * meta.value(QStringLiteral("height")).toInt();
}

// The reply handler must reject malformed metadata before asking whether its shape belongs to the current layout.
// Keeping this decision pure prevents an unusable zero-sized reply from being misclassified as a layout change.
enum class ReplyCheck { UnusableBuffer, LayoutShape };
inline ReplyCheck firstReplyCheck(const QVariantMap &meta) { return rawBufferProblem(meta) ? ReplyCheck::UnusableBuffer : ReplyCheck::LayoutShape; }

// The single scale at which an image covers the logical desktop, or 0 when its shape doesn't fit the desktop.
// KWin 6.6 renders CaptureWorkspace with native-resolution at max(1, largest output scale) for every output, as
// size * scale rounded; Spectacle's full-desktop region result has the same shape. maxScale is the largest screen
// scale Qt reports. An image whose two axes need different scales, or a scale outside 1..maxScale, was rendered for
// another layout (KWin can render a hotplugged desktop before Qt reports the new output).
inline qreal desktopScale(const QSize &image, const QRect &virtualRect, qreal maxScale) {
    if (image.isEmpty() || virtualRect.isEmpty()) return 0;
    // Scales that round to this image width and height, limited to what KWin can use.
    const qreal low = std::max<qreal>({(image.width() - 0.5) / virtualRect.width(), (image.height() - 0.5) / virtualRect.height(), 1.0});
    const qreal high = std::min<qreal>({(image.width() + 0.5) / virtualRect.width(), (image.height() + 0.5) / virtualRect.height(), qMax<qreal>(1, maxScale)});
    if (low > high) return 0;
    return qreal(image.width()) / virtualRect.width();
}

} // namespace CaptureChecks
