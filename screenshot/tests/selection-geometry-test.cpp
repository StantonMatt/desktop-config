// Selection geometry checks for quick-region-overlay. Run: ctest --test-dir <build dir> --output-on-failure
#include "../selection-geometry.h"
#include <QTest>

using namespace SelectionGeometry;

// The live layout: HDMI-A-5 2560x1440 at 0,0, DP-5 3440x1440 at 2560,0, DP-4 1920x1080 at 6000,195, scale 1.
static const QRect desktop(0, 0, 7920, 1440);
static const QSize image(7920, 1440);

class SelectionGeometryTest final : public QObject {
    Q_OBJECT
private slots:
    void allDragDirectionsMatch_data() {
        QTest::addColumn<QPoint>("press");
        QTest::addColumn<QPoint>("release");
        QTest::newRow("down-right") << QPoint(100, 100) << QPoint(150, 130);
        QTest::newRow("up-left") << QPoint(150, 130) << QPoint(100, 100);
        QTest::newRow("up-right") << QPoint(100, 130) << QPoint(150, 100);
        QTest::newRow("down-left") << QPoint(150, 100) << QPoint(100, 130);
    }
    void allDragDirectionsMatch() {
        QFETCH(QPoint, press);
        QFETCH(QPoint, release);
        QCOMPARE(selectionRect(press, release), QRect(100, 100, 51, 31));
        QCOMPARE(selectionCrop(press, release, desktop, image), QRect(100, 100, 51, 31));
    }
    void onePixelInvertedDragsCopy() {
        QCOMPARE(selectionCrop(QPoint(100, 100), QPoint(500, 99), desktop, image), QRect(100, 99, 401, 2));
        QCOMPARE(selectionCrop(QPoint(100, 100), QPoint(99, 400), desktop, image), QRect(99, 100, 2, 301));
        QCOMPARE(selectionCrop(QPoint(100, 100), QPoint(99, 99), desktop, image), QRect(99, 99, 2, 2));
    }
    void zeroSizeClickIsOnePixel() {
        QCOMPARE(selectionRect(QPoint(100, 100), QPoint(100, 100)), QRect(100, 100, 1, 1));
        QCOMPARE(selectionCrop(QPoint(100, 100), QPoint(100, 100), desktop, image), QRect(100, 100, 1, 1));
    }
    void crossMonitorSpans() {
        // HDMI-A-5 into DP-5, both directions.
        QCOMPARE(selectionCrop(QPoint(2400, 300), QPoint(2900, 600), desktop, image), QRect(2400, 300, 501, 301));
        QCOMPARE(selectionCrop(QPoint(2900, 600), QPoint(2400, 300), desktop, image), QRect(2400, 300, 501, 301));
        // The up-right drag from layout-change-2026-10-18.jsonl, which logged 3101x149: DP-5 into DP-4.
        QCOMPARE(selectionCrop(QPoint(3000, 600), QPoint(6100, 450), desktop, image), QRect(3000, 450, 3101, 151));
        // All three monitors.
        QCOMPARE(selectionCrop(QPoint(7919, 1439), QPoint(0, 0), desktop, image), QRect(0, 0, 7920, 1440));
    }
    void clippedToDesktop() {
        QCOMPARE(selectionCrop(QPoint(7900, 1430), QPoint(8100, 1600), desktop, image), QRect(7900, 1430, 20, 10));
        QVERIFY(selectionCrop(QPoint(8000, 1500), QPoint(8100, 1600), desktop, image).isEmpty());
    }
    void scaledCrops() {
        // Scale 2 with a desktop that starts left of the origin.
        const QRect scaled(-100, 0, 200, 50);
        QCOMPARE(selectionCrop(QPoint(-81, 19), QPoint(-90, 10), scaled, QSize(400, 100)), QRect(20, 20, 20, 20));
        QCOMPARE(selectionCrop(QPoint(-90, 10), QPoint(-81, 19), scaled, QSize(400, 100)), QRect(20, 20, 20, 20));
        // Fractional scale rounds outwards: logical 1..3 covers native 1.5..6.
        QCOMPARE(selectionCrop(QPoint(3, 3), QPoint(1, 1), QRect(0, 0, 100, 100), QSize(150, 150)), QRect(1, 1, 5, 5));
        // The far edge clamps to the image.
        QCOMPARE(selectionCrop(QPoint(99, 99), QPoint(90, 90), QRect(0, 0, 100, 100), QSize(150, 150)), QRect(135, 135, 15, 15));
    }
    void fractionalScales_data() {
        QTest::addColumn<int>("imageWidth");    // a 1000x500 desktop at the scale, as KWin renders it
        QTest::addColumn<int>("imageHeight");
        QTest::addColumn<QRect>("crop");        // logical 20,10 to 39,29 (20x20)
        QTest::newRow("1.1") << 1100 << 550 << QRect(22, 11, 22, 22);
        QTest::newRow("1.15") << 1150 << 575 << QRect(23, 11, 23, 24);    // 11.5 .. 34.5 rounds out to 11 .. 35
        QTest::newRow("1.25") << 1250 << 625 << QRect(25, 12, 25, 26);    // 12.5 .. 37.5
        QTest::newRow("1.35") << 1350 << 675 << QRect(27, 13, 27, 28);    // 13.5 .. 40.5
        QTest::newRow("1.5") << 1500 << 750 << QRect(30, 15, 30, 30);
        QTest::newRow("1.75") << 1750 << 875 << QRect(35, 17, 35, 36);    // 17.5 .. 52.5
        QTest::newRow("2") << 2000 << 1000 << QRect(40, 20, 40, 40);
    }
    void fractionalScales() {
        // Edges that land on whole native pixels stay there. With the scale as a double, 20 * 1.15 floored to 22 and
        // 40 * 1.35 ceiled to 55, so crops grew by a pixel.
        QFETCH(int, imageWidth);
        QFETCH(int, imageHeight);
        QFETCH(QRect, crop);
        const QRect logical(0, 0, 1000, 500);
        const QSize size(imageWidth, imageHeight);
        QCOMPARE(selectionCrop(QPoint(20, 10), QPoint(39, 29), logical, size), crop);
        QCOMPARE(selectionCrop(QPoint(39, 29), QPoint(20, 10), logical, size), crop);
        // The whole desktop is the whole image, and the pixel under the press is the crop's corner.
        QCOMPARE(selectionCrop(QPoint(0, 0), QPoint(999, 499), logical, size), QRect(QPoint(0, 0), size));
        QCOMPARE(imagePoint(QPoint(20, 10), logical, size), crop.topLeft());
        // Neighbouring selections overlap by at most the shared, partly covered native column.
        const QRect left = selectionCrop(QPoint(0, 0), QPoint(19, 9), logical, size);
        const QRect right = selectionCrop(QPoint(20, 0), QPoint(39, 9), logical, size);
        QVERIFY(left.right() + 1 >= right.left());
        QVERIFY(left.right() <= right.left());
        // The whole of a live-sized desktop that starts left of the origin.
        const QRect wide(-1920, 0, 7920, 1440);
        const QSize wideImage(qRound(7920.0 * imageWidth / 1000), qRound(1440.0 * imageWidth / 1000));
        QCOMPARE(selectionCrop(QPoint(-1920, 0), QPoint(5999, 1439), wide, wideImage), QRect(QPoint(0, 0), wideImage));
    }
    void imagePoints() {
        QCOMPARE(imagePoint(QPoint(3000, 600), desktop, image), QPoint(3000, 600));
        QCOMPARE(imagePoint(QPoint(9000, -5), desktop, image), QPoint(7919, 0));
        QCOMPARE(imagePoint(QPoint(-90, 10), QRect(-100, 0, 200, 50), QSize(400, 100)), QPoint(20, 20));
    }
    void sourceRects() {
        // DP-5's local 10,20 10x5 is global 2570,20.
        QCOMPARE(sourceRect(QRect(10, 20, 10, 5), QPoint(2560, 0), desktop, image), QRectF(2570, 20, 10, 5));
        QCOMPARE(sourceRect(QRect(10, 20, 10, 5), QPoint(0, 0), QRect(-100, 0, 200, 50), QSize(400, 100)), QRectF(220, 40, 20, 10));
    }
};

QTEST_APPLESS_MAIN(SelectionGeometryTest)
#include "selection-geometry-test.moc"
