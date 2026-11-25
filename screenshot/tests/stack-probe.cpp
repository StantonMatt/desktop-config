// Dedicated editor/MIME receiver for unattended clipboard-stack integration checks.
#include "../stack-clipboard.h"
#include <QApplication>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QDBusConnection>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QBuffer>
#include <QImage>
#include <QMimeData>
#include <QTimer>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QFile>
#include <limits>

using namespace ClipboardStack;
static QJsonObject json(const Item &item) {
    QJsonObject result;
    for (auto it = item.begin(); it != item.end(); ++it) result[it.key()] = QString::fromLatin1(it.value().toBase64());
    return result;
}
class Probe final : public QPlainTextEdit {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.local.StackProbe")
public:
    StackClipboard raw;
    Item source;
    QJsonArray events;
    QJsonObject snapshot;
    Item saved, lastSnapshot;
    bool saveNext = false, savedReady = false;
    quint64 serial = 0;
    bool custom = false;
    bool suppressCopy = false;
    bool ignorePaste = false;
    int pasteDelay = 0;
    explicit Probe() {
        resize(800, 600);
        setWindowTitle(QStringLiteral("QRO dedicated stack test"));
        connect(&raw, &StackClipboard::snapshotReady, this, [this](quint64, const Item &item, const QString &error) {
            lastSnapshot = item;
            snapshot = json(item);
            snapshot[QStringLiteral("_error")] = error;
            if (saveNext) { savedReady = error.isEmpty(); saved = item; saveNext = false; }
        });
    }
    void receivePaste(bool shift) {
        Item item;
        const auto *mime = QApplication::clipboard()->mimeData();
        if (mime) for (const QString &format : mime->formats()) item.insert(format, mime->data(format));
        auto entry = json(item);
        entry[QStringLiteral("_key")] = shift ? QStringLiteral("Ctrl+Shift+V") : QStringLiteral("Ctrl+V");
        events << entry;
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (suppressCopy && event->key() == Qt::Key_C && event->modifiers().testFlag(Qt::ControlModifier)) {
            events << QJsonObject{{QStringLiteral("_copy"), true}, {QStringLiteral("alt"), event->modifiers().testFlag(Qt::AltModifier)}, {QStringLiteral("shift"), event->modifiers().testFlag(Qt::ShiftModifier)}};
            event->accept(); return;
        }
        if (event->key() == Qt::Key_V && event->modifiers().testFlag(Qt::ControlModifier)) {
            if (!ignorePaste) {
                const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
                if (pasteDelay) QTimer::singleShot(pasteDelay, this, [this, shift] { receivePaste(shift); });
                else receivePaste(shift);
            }
            event->accept();
            return;
        }
        if (custom && event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
            raw.offer(source, ++serial);
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
public slots:
    void Prepare(const QString &text) { custom = false; setPlainText(text); selectAll(); setFocus(); }
    bool FocusReady() const { return isActiveWindow() && textCursor().hasSelection() && QApplication::focusWidget() && (QApplication::focusWidget() == this || isAncestorOf(QApplication::focusWidget())); }
    void SuppressCopy(bool suppress) { suppressCopy = suppress; }
    void IgnorePaste(bool ignore) { ignorePaste = ignore; }
    void PasteDelay(int milliseconds) { pasteDelay = milliseconds; }
    void Custom(const QString &mode, const QString &value) {
        source.clear(); custom = true;
        if (mode == QLatin1String("image")) {
            QImage image(32, 24, QImage::Format_ARGB32); image.fill(QColor(value));
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
            source.insert(QStringLiteral("image/png"), bytes);
        } else if (mode == QLatin1String("rich")) {
            source.insert(QStringLiteral("text/plain"), value.toUtf8());
            source.insert(QStringLiteral("text/html"), (QStringLiteral("<b>") + value.toHtmlEscaped() + QStringLiteral("</b>")).toUtf8());
        } else if (mode == QLatin1String("files")) {
            source.insert(QStringLiteral("text/uri-list"), value.toUtf8());
            source.insert(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("0"));
        } else if (mode == QLatin1String("secret")) {
            source.insert(QStringLiteral("text/plain"), value.toUtf8());
            source.insert(secretHint, QByteArrayLiteral("secret"));
        } else {
            source.insert(QStringLiteral("text/plain"), value.toUtf8());
            source.insert(QStringLiteral("application/x-qro-test"), QByteArray::fromHex("0001feff"));
        }
    }
    void Offer() { raw.offer(source, ++serial); }
    bool SourceIsSecret() const { return custom && source.value(secretHint) == QByteArrayLiteral("secret"); }
    void ReadClipboard() { snapshot = {}; raw.snapshot(++serial, std::numeric_limits<qint64>::max()); }
    void SaveClipboard() { savedReady = false; saveNext = true; raw.snapshot(++serial, std::numeric_limits<qint64>::max()); }
    QString SavedState() const { return savedReady ? QStringLiteral("ready:") + QString::fromLatin1(QCryptographicHash::hash(encode(saved), QCryptographicHash::Sha256).toHex()) : saveNext ? QStringLiteral("pending") : QStringLiteral("error:") + snapshot.value(QStringLiteral("_error")).toString(); }
    QString Digest() const { return snapshot.contains(QStringLiteral("_error")) ? QString::fromLatin1(QCryptographicHash::hash(encode(lastSnapshot), QCryptographicHash::Sha256).toHex()) : QString(); }
    void RestoreSaved() { if (savedReady) raw.offer(saved, ++serial); }
    bool WriteSaved(const QString &path) {
        if (!savedReady) return false;
        QSaveFile file(path);
        const auto bytes = encode(saved);
        return file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            && file.write(bytes) == bytes.size() && file.commit();
    }
    QString Clipboard() const { return QString::fromUtf8(QJsonDocument(snapshot).toJson(QJsonDocument::Compact)); }
    QString Events() const { return QString::fromUtf8(QJsonDocument(events).toJson(QJsonDocument::Compact)); }
    void ResetEvents() { events = {}; }
    void Quit() { qApp->quit(); }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setQuitLockEnabled(false);
    app.setDesktopFileName(app.arguments().contains(QStringLiteral("--live")) ? QStringLiteral("qro-stack-live-probe") : app.arguments().contains(QStringLiteral("--terminal")) ? QStringLiteral("org.kde.konsole") : QStringLiteral("qro-stack-probe"));
    Probe probe;
    const int serviceArg = app.arguments().indexOf(QStringLiteral("--service"));
    const QString service = serviceArg >= 0 ? app.arguments().value(serviceArg + 1) : QStringLiteral("org.local.StackProbe");
    if (!QDBusConnection::sessionBus().registerService(service)) return 3;
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/"), &probe, QDBusConnection::ExportAllSlots);
    if (!app.arguments().contains(QStringLiteral("--hidden"))) { probe.show(); probe.activateWindow(); }
    return app.exec();
}
#include "stack-probe.moc"
