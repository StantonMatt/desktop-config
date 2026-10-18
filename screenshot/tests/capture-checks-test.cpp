// Capture checks for quick-region-overlay. Run: ctest --test-dir <build dir> --output-on-failure
#include "../capture-checks.h"
#include <QTest>

using namespace CaptureChecks;

// The live layout: HDMI-A-5 2560x1440 at 0,0, DP-5 3440x1440 at 2560,0, DP-4 1920x1080 at 6000,195, scale 1.
static const QRect desktop(0, 0, 7920, 1440);

static QVariantMap raw(int width, int height, int stride, QImage::Format format) {
    return {{QStringLiteral("type"), QStringLiteral("raw")}, {QStringLiteral("width"), width}, {QStringLiteral("height"), height},
            {QStringLiteral("stride"), stride}, {QStringLiteral("format"), uint(format)}};
}

class CaptureChecksTest final : public QObject {
    Q_OBJECT
private slots:
    void validRawBuffers() {
        // What KWin 6.6 sends for the live desktop.
        QCOMPARE(rawBufferSize(raw(7920, 1440, 7920 * 4, QImage::Format_ARGB32_Premultiplied)), qint64(7920) * 4 * 1440);
        // Padded rows are fine; the size covers the padding.
        QCOMPARE(rawBufferSize(raw(10, 2, 64, QImage::Format_RGB32)), qint64(128));
        // 24-bit rows: 3 bytes per pixel.
        QCOMPARE(rawBufferSize(raw(10, 2, 30, QImage::Format_RGB888)), qint64(60));
    }
    void rejectedRawBuffers() {
        QCOMPARE(rawBufferSize(raw(10, 2, 39, QImage::Format_ARGB32)), qint64(0));    // stride below width * 4
        QCOMPARE(rawBufferSize(raw(10, 2, 29, QImage::Format_RGB888)), qint64(0));    // stride below width * 3
        QCOMPARE(rawBufferSize(raw(0, 2, 40, QImage::Format_ARGB32)), qint64(0));     // empty
        QCOMPARE(rawBufferSize(raw(10, -1, 40, QImage::Format_ARGB32)), qint64(0));   // negative height
        QCOMPARE(rawBufferSize(raw(10, 2, -40, QImage::Format_ARGB32)), qint64(0));   // negative stride
        QCOMPARE(rawBufferSize(raw(10, 2, 40, QImage::Format_Invalid)), qint64(0));
        QCOMPARE(rawBufferSize(raw(10, 2, 40, QImage::NImageFormats)), qint64(0));
        QCOMPARE(rawBufferSize(raw(10, 2, 40, QImage::Format(1000))), qint64(0));
        auto png = raw(10, 2, 40, QImage::Format_ARGB32);
        png[QStringLiteral("type")] = QStringLiteral("png");
        QCOMPARE(rawBufferSize(png), qint64(0));
        QCOMPARE(rawBufferSize({}), qint64(0));
    }
    void rawBufferProblems() {
        QCOMPARE(rawBufferProblem(raw(10, 2, 40, QImage::Format_ARGB32)), nullptr);
        QCOMPARE(QByteArray(rawBufferProblem(raw(10, 2, 39, QImage::Format_ARGB32))), QByteArray("stride"));
        QCOMPARE(QByteArray(rawBufferProblem(raw(0, 2, 40, QImage::Format_ARGB32))), QByteArray("size"));
        QCOMPARE(QByteArray(rawBufferProblem(raw(10, 2, 40, QImage::Format(1000)))), QByteArray("format"));
        QCOMPARE(QByteArray(rawBufferProblem({})), QByteArray("type"));
    }
    void captureShapeMatchesLayout() {
        QCOMPARE(desktopScale(QSize(7920, 1440), desktop, 1), 1.0);
        // KWin renders every output at the largest scale, rounding size * scale.
        QCOMPARE(desktopScale(QSize(15840, 2880), desktop, 2), 2.0);
        QCOMPARE(desktopScale(QSize(2399, 1351), QRect(0, 0, 1919, 1081), 1.25), 2399.0 / 1919);   // 2398.75 x 1351.25
        // A screen scale below 1 still renders at 1.
        QCOMPARE(desktopScale(QSize(1920, 1080), QRect(0, 0, 1920, 1080), 0.75), 1.0);
    }
    void captureShapeFromAnotherLayout() {
        // DP-4 hotplugged: KWin rendered the new 7920-wide desktop while Qt still knew only the first two monitors.
        QCOMPARE(desktopScale(QSize(7920, 1440), QRect(0, 0, 6000, 1440), 1), 0.0);
        // DP-4 unplugged: the old desktop against the new layout.
        QCOMPARE(desktopScale(QSize(6000, 1440), desktop, 1), 0.0);
        // A monitor added below: both axes grew, by different factors.
        QCOMPARE(desktopScale(QSize(7920, 2520), desktop, 1), 0.0);
        // Same shape but a scale no screen has.
        QCOMPARE(desktopScale(QSize(15840, 2880), desktop, 1), 0.0);
        QCOMPARE(desktopScale(QSize(3960, 720), desktop, 1), 0.0);
        QCOMPARE(desktopScale(QSize(), desktop, 1), 0.0);
    }
    void malformedReplyIsCheckedBeforeLayout() {
        // A zero-sized reply would otherwise look like a layout mismatch and be retried for two seconds.
        QCOMPARE(firstReplyCheck(raw(0, 2, 40, QImage::Format_ARGB32)), ReplyCheck::UnusableBuffer);
        QCOMPARE(firstReplyCheck(raw(7920, 1440, 7920 * 4, QImage::Format_ARGB32)), ReplyCheck::LayoutShape);
    }
};

QTEST_APPLESS_MAIN(CaptureChecksTest)
#include "capture-checks-test.moc"
