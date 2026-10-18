// Native Wayland clipboard receiver: on every clipboard change, requests image/png over ext-data-control and decodes it.
#include <QGuiApplication>
#include <QDateTime>
#include <QImage>
#include <QMimeData>
#include <KSystemClipboard>
#include <cstdio>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    auto *cb = KSystemClipboard::instance();
    QObject::connect(cb, &KSystemClipboard::changed, [cb](QClipboard::Mode mode) {
        if (mode != QClipboard::Clipboard) return;
        const qint64 changed = QDateTime::currentMSecsSinceEpoch();
        const QMimeData *m = cb->mimeData(QClipboard::Clipboard);
        if (!m || !m->hasFormat(QStringLiteral("image/png"))) { printf("%lld changed-no-png\n", changed); fflush(stdout); return; }
        const QImage img = QImage::fromData(m->data(QStringLiteral("image/png")), "PNG");
        printf("%lld %lld %dx%d\n", changed, QDateTime::currentMSecsSinceEpoch(), img.width(), img.height()); fflush(stdout);
    });
    printf("ready\n"); fflush(stdout);
    return app.exec();
}
