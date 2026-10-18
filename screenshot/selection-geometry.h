// Pure geometry shared by quick-region-overlay.cpp and tests/selection-geometry-test.cpp.
// Global positions are logical desktop pixels; the frozen image covers virtualRect with imageSize native pixels, so
// one logical pixel is imageSize / virtualRect.size() native pixels. Crops use exact integer arithmetic on those sizes:
// a scale such as 1.15 has no exact double, and multiplying by it moved edges that land on whole pixels by one.
#pragma once
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QtGlobal>
#include <algorithm>

namespace SelectionGeometry {

// floor(a / b) and ceil(a / b) for b > 0.
inline qint64 floorDiv(qint64 a, qint64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline qint64 ceilDiv(qint64 a, qint64 b) { return a >= 0 ? (a + b - 1) / b : -(-a / b); }

// The native coordinate of a logical offset from the desktop's edge, rounded down or up and clamped to 0..imageLength.
inline int nativeFloor(int offset, int virtualLength, int imageLength) {
    return int(std::clamp<qint64>(floorDiv(qint64(offset) * imageLength, virtualLength), 0, imageLength));
}
inline int nativeCeil(int offset, int virtualLength, int imageLength) {
    return int(std::clamp<qint64>(ceilDiv(qint64(offset) * imageLength, virtualLength), 0, imageLength));
}

// The selection includes both the press and the release pixel in every drag direction. QRect(a, b).normalized() does
// not: Qt 6 drops both endpoints of an inverted rect, so up/left drags came out 2 px smaller and a drag inverted by
// 1 px on one axis came out empty.
inline QRect selectionRect(const QPoint &press, const QPoint &current) {
    return QRect(QPoint(qMin(press.x(), current.x()), qMin(press.y(), current.y())),
                 QPoint(qMax(press.x(), current.x()), qMax(press.y(), current.y())));
}

// Native image pixels covered by a logical rect, rounded outwards and clipped to the image.
inline QRect imageRect(const QRect &logical, const QRect &virtualRect, const QSize &imageSize) {
    if (logical.isEmpty() || virtualRect.isEmpty()) return {};
    const int left = nativeFloor(logical.left() - virtualRect.left(), virtualRect.width(), imageSize.width());
    const int top = nativeFloor(logical.top() - virtualRect.top(), virtualRect.height(), imageSize.height());
    const int right = nativeCeil(logical.right() + 1 - virtualRect.left(), virtualRect.width(), imageSize.width());
    const int bottom = nativeCeil(logical.bottom() + 1 - virtualRect.top(), virtualRect.height(), imageSize.height());
    return QRect(left, top, qMax(0, right - left), qMax(0, bottom - top));
}

// The native crop a drag from press to release copies; empty when it misses the desktop.
inline QRect selectionCrop(const QPoint &press, const QPoint &release, const QRect &virtualRect, const QSize &imageSize) {
    return imageRect(selectionRect(press, release).intersected(virtualRect), virtualRect, imageSize);
}

// The native pixel under a logical point, clamped to the image.
inline QPoint imagePoint(const QPoint &global, const QRect &virtualRect, const QSize &imageSize) {
    if (virtualRect.isEmpty() || imageSize.isEmpty()) return {};
    return QPoint(qMin(nativeFloor(global.x() - virtualRect.left(), virtualRect.width(), imageSize.width()), imageSize.width() - 1),
                  qMin(nativeFloor(global.y() - virtualRect.top(), virtualRect.height(), imageSize.height()), imageSize.height() - 1));
}

// Source rect in the frozen image for a rect local to the overlay on the output at outputOrigin. Painting may use a
// fractional source, so this one stays in floating point.
inline QRectF sourceRect(const QRect &local, const QPoint &outputOrigin, const QRect &virtualRect, const QSize &imageSize) {
    if (virtualRect.isEmpty()) return {};
    const qreal scaleX = qreal(imageSize.width()) / virtualRect.width();
    const qreal scaleY = qreal(imageSize.height()) / virtualRect.height();
    return QRectF((outputOrigin.x() + local.x() - virtualRect.left()) * scaleX, (outputOrigin.y() + local.y() - virtualRect.top()) * scaleY,
                  local.width() * scaleX, local.height() * scaleY);
}

} // namespace SelectionGeometry
