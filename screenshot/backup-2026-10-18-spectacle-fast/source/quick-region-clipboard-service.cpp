// Fast region capture for Plasma/Wayland. Uses the installed Spectacle, unmodified.
#include <KSystemClipboard>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QProcess>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <csignal>
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

static const QString spectacleName = QStringLiteral("org.kde.spectacle");
static void report(const QString &message)
{
    qInfo().noquote() << QString::number(QDateTime::currentMSecsSinceEpoch()) << message;
}

class RegionService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.local.QuickRegion")
public:
    explicit RegionService(QObject *parent = nullptr) : QObject(parent)
    {
        runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        capture = runtime + QStringLiteral("/quick-region-screenshot.png");
        // IN_CLOSE_WRITE avoids observing half-written PNGs. MOVED_TO supports atomic saves.
        notifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (notifyFd < 0 || inotify_add_watch(notifyFd, QFile::encodeName(runtime).constData(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0)
            qFatal("Cannot watch screenshot directory");
        notifier = new QSocketNotifier(notifyFd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, &RegionService::readEvents);
        auto clipboard = KSystemClipboard::instance(); // ext-data-control: no focus/serial required.
        connect(clipboard, &KSystemClipboard::changed, this, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard && awaitingOffer) {
                const auto data = KSystemClipboard::instance()->mimeData(mode);
                // Observe the local API change; end-to-end timing uses a receiving app.
                // Never log or persist clipboard content.
                if (data && data->data(QStringLiteral("image/png")) == offeredPng) {
                    report(QStringLiteral("clipboard-set png_bytes=%1").arg(offeredPng.size()));
                    awaitingOffer = false;
                    offeredPng.clear();
                }
            }
        });
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        env.insert(QStringLiteral("QSG_RENDER_LOOP"), QStringLiteral("threaded"));
        spectacle.setProcessEnvironment(env);
        spectacle.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        connect(&spectacle, &QProcess::started, this, [this] {
            report(QStringLiteral("spectacle-start pid=%1 mode=%2").arg(spectacle.processId()).arg(oneShot ? "independent" : "idle-dbus"));
        });
        connect(&spectacle, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
            report(QStringLiteral("spectacle-exit code=%1").arg(code));
            busy = false;
            QTimer::singleShot(30, this, &RegionService::warm);
        });
        connect(&spectacle, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            report(QStringLiteral("spectacle-error %1").arg(spectacle.errorString()));
            if (error == QProcess::FailedToStart) { busy = false; pending = false; }
        });
        watcher = new QDBusServiceWatcher(spectacleName, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &, const QString &owner) {
            if (!owner.isEmpty() && pending) QTimer::singleShot(0, this, &RegionService::dispatch);
            if (owner.isEmpty()) QTimer::singleShot(30, this, &RegionService::warm);
        });
        // A stale file left by an interrupted capture must never replace the clipboard at login.
        QFile::remove(capture);
        QTimer::singleShot(0, this, &RegionService::warm);
    }
    ~RegionService() override
    {
        if (spectacle.state() != QProcess::NotRunning) {
            spectacle.terminate();
            if (!spectacle.waitForFinished(1500)) { spectacle.kill(); spectacle.waitForFinished(1000); }
        }
        if (notifyFd >= 0) close(notifyFd);
    }
public Q_SLOTS:
    void Capture()
    {
        if (busy || pending) { report(QStringLiteral("capture-already-active")); return; }
        report(QStringLiteral("capture-request"));
        pending = true;
        dispatch();
    }
private:
    QString runtime, capture;
    int notifyFd = -1;
    QSocketNotifier *notifier = nullptr;
    QDBusServiceWatcher *watcher = nullptr;
    QProcess spectacle;
    bool busy = false, pending = false, oneShot = false, awaitingOffer = false;
    QByteArray offeredPng;
    qint64 retryUntil = 0;

    QStringList captureArguments() const
    {
        return {QStringLiteral("--region"), QStringLiteral("--background"), QStringLiteral("--nonotify"), QStringLiteral("--output"), capture};
    }
    void warm()
    {
        if (spectacle.state() != QProcess::NotRunning) return;
        if (QDBusConnection::sessionBus().interface()->isServiceRegistered(spectacleName).value()) {
            // Leave an independently opened Spectacle alone; use -i for our next capture.
            if (pending) dispatch();
            return;
        }
        oneShot = false;
        spectacle.start(QStringLiteral("/usr/bin/spectacle"), {QStringLiteral("--desktopfile"), spectacleName, QStringLiteral("--dbus")});
    }
    void dispatch()
    {
        if (!pending) return;
        auto bus = QDBusConnection::sessionBus();
        const auto ownerPid = bus.interface()->servicePid(spectacleName);
        if (!ownerPid.isValid()) {
            warm(); // Owner-change notification dispatches once startup finishes.
            return;
        }
        if (spectacle.state() != QProcess::NotRunning && ownerPid.value() == spectacle.processId()) {
            auto message = QDBusMessage::createMethodCall(spectacleName, QStringLiteral("/org/kde/spectacle"), QStringLiteral("org.kde.KDBusService"), QStringLiteral("CommandLine"));
            message << (QStringList{QStringLiteral("/usr/bin/spectacle")} + captureArguments()) << QDir::homePath() << QVariantMap{};
            auto call = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
            connect(call, &QDBusPendingCallWatcher::finished, this, [this, call] {
                QDBusPendingReply<int> reply = *call;
                if (reply.isError()) { report(QStringLiteral("capture-dbus-error %1").arg(reply.error().message())); busy = false; }
                call->deleteLater();
            });
            busy = true;
            pending = false;
            report(QStringLiteral("capture-dispatched warm"));
        } else if (spectacle.state() == QProcess::NotRunning) {
            // Never forward --background to an unrelated Spectacle or close its viewer.
            oneShot = true;
            pending = false;
            busy = true;
            spectacle.start(QStringLiteral("/usr/bin/spectacle"), QStringList{QStringLiteral("--desktopfile"), spectacleName, QStringLiteral("--new-instance")} + captureArguments());
            report(QStringLiteral("capture-dispatched independent"));
        }
    }
    void readEvents()
    {
        alignas(inotify_event) char buffer[65536];
        ssize_t length;
        bool completed = false;
        while ((length = read(notifyFd, buffer, sizeof(buffer))) > 0) {
            for (ssize_t offset = 0; offset < length;) {
                const auto event = reinterpret_cast<const inotify_event *>(buffer + offset);
                if ((event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) && event->len && QByteArray(event->name) == "quick-region-screenshot.png") completed = true;
                offset += sizeof(inotify_event) + event->len;
            }
        }
        if (!completed) return;
        QFile file(capture);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QByteArray png = file.readAll();
        file.close();
        QImageReader reader(capture);
        const auto size = reader.size();
        QRect desktop;
        for (auto screen : QGuiApplication::screens()) desktop |= screen->geometry();
        if (size == desktop.size()) {
            QFile::remove(capture);
            report(QStringLiteral("discarded-empty-selection %1x%2").arg(size.width()).arg(size.height()));
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if (now >= retryUntil) {
                retryUntil = now + 60000;
                QTimer::singleShot(750, this, &RegionService::Capture);
            }
            return;
        }
        // Decode once for Qt image consumers. KDE serves the image over ext-data-control.
        const auto image = QImage::fromData(png, "PNG");
        if (size.isEmpty() || image.isNull()) {
            report(QStringLiteral("invalid-capture ignored"));
            QFile::remove(capture);
            return;
        }
        QFile::remove(capture);
        retryUntil = 0;
        auto mime = new QMimeData;
        // Qt image consumers expect the image marker as well as the encoded PNG.
        mime->setImageData(image);
        mime->setData(QStringLiteral("image/png"), png);
        offeredPng = png;
        awaitingOffer = true;
        KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
        report(QStringLiteral("clipboard-offer %1x%2 png_bytes=%3").arg(size.width()).arg(size.height()).arg(png.size()));
    }
};

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Quick Region Clipboard"));
    app.setQuitOnLastWindowClosed(false);
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.local.QuickRegion"))) qFatal("Quick Region service already running");
    RegionService service;
    if (!bus.registerObject(QStringLiteral("/"), &service, QDBusConnection::ExportAllSlots)) qFatal("Cannot register capture endpoint");
    report(QStringLiteral("service-ready platform=%1 pid=%2").arg(app.platformName()).arg(QCoreApplication::applicationPid()));
    // SIGTERM is delivered through an fd so cleanup runs safely in Qt's event loop.
    int signalPipe[2];
    if (pipe2(signalPipe, O_NONBLOCK | O_CLOEXEC) != 0) qFatal("Cannot create signal pipe");
    static int signalFd;
    signalFd = signalPipe[1];
    auto handler = +[](int) { const char byte = 1; (void)!write(signalFd, &byte, 1); };
    std::signal(SIGTERM, handler);
    std::signal(SIGINT, handler);
    QSocketNotifier signalNotifier(signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
    return app.exec();
}
#include "quick-region-clipboard-service.moc"
