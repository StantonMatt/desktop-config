#include "stack-busy.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPainter>
#include <QScreen>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>
#include <QWidget>
#include <QWindow>
#include <LayerShellQt/window.h>

namespace {
const QString pointerScript = QStringLiteral("quick-region-overlay-stack-pointer");
QDBusMessage scripting(const QString &method) {
    return QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"), method);
}
class Wheel final : public QWidget {
public:
    qreal angle = 0;
    explicit Wheel(QScreen *screen) {
        setWindowTitle(QStringLiteral("Clipboard stack busy"));
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        resize(28, 28);
        winId();
        windowHandle()->setScreen(screen);
        auto *layer = LayerShellQt::Window::get(windowHandle());
        layer->setScreen(screen);
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorLeft));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setActivateOnShow(false);
        // KWin treats unknown scopes as normal windows and fades their deleted surfaces out. Utility
        // surfaces bypass its normal-window and popup animations, so hide/output changes are immediate.
        layer->setScope(QStringLiteral("utility"));
        // Qt's Wayland backend turns WindowTransparentForInput into an empty wl_surface input region.
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(14, 14);
        painter.rotate(angle);
        const QRectF arc(-9.5, -9.5, 19, 19);
        painter.setPen(QPen(QColor(0, 0, 0, 100), 5, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arc, 0, 270 * 16);
        // A non-activating tool window uses the inactive palette group, whose highlight may be grey.
        painter.setPen(QPen(palette().color(QPalette::Active, QPalette::Highlight), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arc, 0, 270 * 16);
    }
};
}

StackBusy::StackBusy(QObject *parent) : QObject(parent) {
    unload(pointerScript); // Retire a tracker left by an interrupted daemon before accepting shortcuts.
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/StackBusy"), this, QDBusConnection::ExportAllSlots);
    delay.setSingleShot(true);
    delay.setTimerType(Qt::PreciseTimer);
    connect(&delay, &QTimer::timeout, this, &StackBusy::startTracking);
    animation.setInterval(17); // At most 59 fps; only the visible 28 x 28 surface repaints.
    animation.setTimerType(Qt::PreciseTimer);
    connect(&animation, &QTimer::timeout, this, [this] {
        if (!visible) return;
        static_cast<Wheel *>(visible.data())->angle = (rotation.elapsed() % 1000) * 0.36;
        visible->update();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) { delete windows.take(screen); });
}
StackBusy::~StackBusy() {
    setBusy(false);
    qDeleteAll(windows);
}
void StackBusy::unload(const QString &name) {
    if (name.isEmpty()) return;
    auto message = scripting(QStringLiteral("unloadScript"));
    message << name;
    QDBusConnection::sessionBus().asyncCall(message, 1000);
}
void StackBusy::setBusy(bool busy) {
    if (active == busy) return;
    active = busy;
    if (busy) {
        rotation.start();
        delay.start(150);
        return;
    }
    delay.stop();
    animation.stop();
    token.clear(); // Ignore already queued cursor reports from the retired script.
    for (auto *window : windows) window->hide();
    visible.clear();
    unload(scriptName);
    scriptName.clear();
    trackerFile.clear();
}
void StackBusy::startTracking() {
    if (!active) return;
    token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    scriptName = pointerScript;
    // Retain each file until its asynchronous load completes, even if another busy period starts meanwhile.
    auto file = QSharedPointer<QTemporaryFile>::create(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/qro-pointer-XXXXXX.js"));
    if (!file->open()) { qWarning() << "stack-busy-pointer-file-failed"; return; }
    trackerFile = file;
    file->write(QStringLiteral(
        "var timer = new QTimer(); timer.interval = 17; timer.singleShot = true;\n"
        "function report() { var p = workspace.cursorPos; callDBus('org.local.QuickRegionOverlay', '/StackBusy', 'org.local.QuickRegionOverlay.StackBusy', 'CursorPosition', '%1', p.x, p.y); }\n"
        "timer.timeout.connect(report);\n"
        "workspace.cursorPosChanged.connect(function() { if (!timer.active) timer.start(); });\n"
        "report();\n").arg(token).toUtf8());
    file->flush();
    auto message = scripting(QStringLiteral("loadScript"));
    message << file->fileName() << scriptName;
    const QString current = token;
    auto *watch = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
    connect(watch, &QDBusPendingCallWatcher::finished, this, [this, watch, file, current] {
        const QDBusPendingReply<int> reply = *watch;
        if (!reply.isError() && reply.value() >= 0) {
            if (active && token == current) {
                auto run = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(reply.value()), QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run"));
                auto *started = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(run, 1000), this);
                connect(started, &QDBusPendingCallWatcher::finished, this, [started, file] {
                    const QDBusPendingReply<> result = *started;
                    if (result.isError()) qWarning() << "stack-busy-pointer-start-failed" << result.error().message();
                    started->deleteLater();
                });
            }
            // setBusy(false) already queued unload after load on this same D-Bus connection. Do not
            // unload here: another busy period may have queued its replacement under the same name.
        } else qWarning() << "stack-busy-pointer-unavailable" << reply.error().message();
        watch->deleteLater();
    });
}
void StackBusy::CursorPosition(const QString &current, int x, int y) {
    if (!active || token.isEmpty() || current != token) return;
    auto *screen = QGuiApplication::screenAt(QPoint(x, y));
    if (!screen) { if (visible) visible->hide(); visible.clear(); return; }
    auto *&window = windows[screen];
    if (!window) window = new Wheel(screen);
    if (visible && visible != window) visible->hide();
    const QPoint local = QPoint(x, y) - screen->geometry().topLeft();
    const int left = qBound(0, local.x() + 16, qMax(0, screen->geometry().width() - window->width()));
    const int top = qBound(0, local.y() + 16, qMax(0, screen->geometry().height() - window->height()));
    LayerShellQt::Window::get(window->windowHandle())->setMargins(QMargins(left, top, 0, 0));
    visible = window;
    window->show();
    if (!animation.isActive()) animation.start();
}
