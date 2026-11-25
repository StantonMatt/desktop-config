#include "stack-controller.h"
#include "stack-holder.h"
#include <QBuffer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits>

using namespace ClipboardStack;
namespace {
// EVIOCGKEY is a state query, not an event reader: no keystrokes are collected or retained.
enum class ModifierState { Released, Held, Unavailable };
ModifierState physicalModifiers() {
    bool keyboard = false;
    constexpr int modifierKeys[]{KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTMETA, KEY_RIGHTMETA};
    const auto bit = [](const unsigned long *bits, int key) { return (bits[key / (8 * sizeof(long))] >> (key % (8 * sizeof(long)))) & 1UL; };
    for (const auto &name : QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("event*")}, QDir::System)) {
        const int fd = open(QFile::encodeName(QStringLiteral("/dev/input/") + name).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long supported[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
        unsigned long state[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
        bool relevant = false;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof supported), supported) >= 0)
            for (int key : modifierKeys) relevant = relevant || bit(supported, key);
        if (relevant) {
            if (ioctl(fd, EVIOCGKEY(sizeof state), state) < 0) { close(fd); return ModifierState::Unavailable; }
            keyboard = true;
            for (int key : modifierKeys) {
                if (bit(state, key)) { close(fd); return ModifierState::Held; }
            }
        }
        close(fd);
    }
    return keyboard ? ModifierState::Released : ModifierState::Unavailable;
}
QDBusMessage call(const QString &service, const QString &path, const QString &interface, const QString &method) {
    return QDBusMessage::createMethodCall(service, path, interface, method);
}
bool locked() {
    const auto reply = QDBusConnection::sessionBus().call(call(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"), QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("GetActive")), QDBus::Block, 500);
    // A missing service is normal in a virtual session without a lock screen. Other failures are uncertain state:
    // refuse input rather than risk pasting into a lock screen while its service is not responding.
    if (reply.type() == QDBusMessage::ErrorMessage)
        return reply.errorName() != QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown") && reply.errorName() != QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner");
    return reply.arguments().size() != 1 || reply.arguments()[0].metaType() != QMetaType::fromType<bool>() || reply.arguments()[0].toBool();
}
}

StackController::StackController(QObject *parent) : QObject(parent), clipboard(this) {
    qRegisterMetaType<ClipboardStack::Item>();
    directory = stateDirectory();
    cleanupClipboardHolders();
    load();
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Stack"), this, QDBusConnection::ExportAllSlots);
    deadline.setSingleShot(true);
    grace.setSingleShot(true);
    modifiers.setInterval(15);
    connect(&modifiers, &QTimer::timeout, this, [this] {
        const auto state = physicalModifiers();
        if (state == ModifierState::Released) {
            modifiers.stop();
            if (!sameTarget() || locked()) { finish(QStringLiteral("Focus changed")); return; }
            auto next = std::move(afterModifiers);
            next();
        } else if (state == ModifierState::Unavailable || modifierTime.elapsed() >= 1500) {
            finish(state == ModifierState::Unavailable ? QStringLiteral("Keyboard state unavailable") : QStringLiteral("Release modifier keys"));
        }
    });
    connect(&keys, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        keyRunning = false;
        if (code != 0 || status != QProcess::NormalExit) {
            releaseSyntheticKeys();
            if (operation == Operation::Pasting) restore(QStringLiteral("Paste key failed"));
            else finish(QStringLiteral("Copy key failed"));
            return;
        }
        auto next = std::move(afterKeys);
        if (next) next();
    });
    connect(&keys, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        keyRunning = false;
        releaseSyntheticKeys();
        if (operation == Operation::Pasting) restore(QStringLiteral("ydotool unavailable"));
        else finish(QStringLiteral("ydotool unavailable"));
    });
    connect(&deadline, &QTimer::timeout, this, [this] {
        if (keyRunning) { keys.kill(); return; }
        if (operation == Operation::Copying) finish(QStringLiteral("Nothing copied"));
        else if (operation == Operation::Pasting || operation == Operation::Offering) restore(QStringLiteral("Paste stopped: no read"));
    });
    connect(&clipboard, &StackClipboard::changed, this, [this] {
        if (operation == Operation::Copying) {
            copyChanged = true;
            if (!keyRunning) { operation = Operation::CopySnapshot; deadline.stop(); takeSnapshot(); }
        }
    });
    connect(&clipboard, &StackClipboard::snapshotReady, this, [this](quint64 id, const Item &item, const QString &error) {
        if (id != serial) return;
        if (!error.isEmpty()) { finish(error); return; }
        if (operation == Operation::CopySnapshot) {
            operation = Operation::Idle;
            if (!restorePending) original.clear();
            if (item.isEmpty()) osd(QStringLiteral("Nothing copied")); else append(item);
        } else if (operation == Operation::RestoreSnapshot) {
            original = item;
            restorePending = true;
            pasteItems = plan(items);
            pasteIndex = 0;
            nextPaste();
        }
    });
    connect(&clipboard, &StackClipboard::offered, this, [this](quint64 id, bool ok) {
        if (id != serial) return;
        if (operation == Operation::Restoring) {
            if (ok) restorePending = false;
            finish(ok ? completion : QStringLiteral("Clipboard restore failed")); return;
        }
        if (operation != Operation::Offering) return;
        if (!ok) { restore(QStringLiteral("Clipboard offer failed")); return; }
        if (!sameTarget() || locked()) { restore(QStringLiteral("Focus changed")); return; }
        operation = Operation::Pasting;
        readSeen = false;
        key(false, kind(pasteItems[pasteIndex]) == Kind::Image, [this] {
            if (readSeen) grace.start(100);
        });
    });
    connect(&clipboard, &StackClipboard::consumed, this, [this](quint64 id, const QString &format) {
        if (id != serial || operation != Operation::Pasting) return;
        readSeen = true;
        qInfo().noquote() << "stack-consumed" << pasteIndex << format;
        if (!keyRunning) grace.start(100);
    });
    connect(&grace, &QTimer::timeout, this, [this] {
        if (operation != Operation::Pasting) return;
        deadline.stop();
        ++pasteIndex;
        nextPaste();
    });

    // KWin 6's public D-Bus API has no non-interactive active-window query. One small script monitors it.
    scriptName = QStringLiteral("quick-region-overlay-stack-focus");
    auto unload = call(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("unloadScript"));
    unload << scriptName;
    QDBusConnection::sessionBus().call(unload, QDBus::Block, 500);
    script.setFileTemplate(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/qro-focus-XXXXXX.js"));
    if (script.open()) {
        script.write("function report(w) { callDBus('org.local.QuickRegionOverlay', '/Stack', 'org.local.QuickRegionOverlay.Stack', 'ActiveWindow', w ? String(w.internalId) : '', w ? String(w.resourceClass) : ''); }\nworkspace.windowActivated.connect(report); report(workspace.activeWindow);\n");
        script.flush();
        auto message = call(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("loadScript"));
        message << script.fileName() << scriptName;
        auto *watch = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
        connect(watch, &QDBusPendingCallWatcher::finished, this, [this, watch] {
            const QDBusPendingReply<int> reply = *watch;
            if (!reply.isError() && reply.value() >= 0) {
                QDBusConnection::sessionBus().asyncCall(call(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(reply.value()), QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run")), 1000);
            } else qWarning() << "stack-focus-unavailable; using normal copy/paste keys";
            watch->deleteLater();
        });
    }
}
StackController::~StackController() {
    shutdown();
    auto message = call(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("unloadScript"));
    message << scriptName;
    QDBusConnection::sessionBus().call(message, QDBus::Block, 500);
    if (keys.state() != QProcess::NotRunning) { keys.kill(); keys.waitForFinished(1000); releaseSyntheticKeys(); }
    if (releaseKeys.state() != QProcess::NotRunning && !releaseKeys.waitForFinished(1000)) { releaseKeys.kill(); releaseKeys.waitForFinished(250); }
}
void StackController::shutdown() {
    if (operation == Operation::Stopping) return;
    operation = Operation::Stopping;
    ++serial; // Invalidate all queued offer/snapshot/consumer callbacks before stopping key work.
    modifiers.stop(); deadline.stop(); grace.stop();
    afterKeys = {}; afterModifiers = {};
    keys.disconnect(this);
    if (keyRunning || keys.state() != QProcess::NotRunning) {
        if (keys.state() != QProcess::NotRunning) { keys.kill(); keys.waitForFinished(250); }
        releaseSyntheticKeys();
    }
    keyRunning = false;
    // The worker remains alive serving the current offer while systemd starts the independent holder.
    const auto owned = clipboard.ownedItem();
    if (owned) handoffClipboard(restorePending ? original : *owned);
}
void StackController::ActiveWindow(const QString &id, const QString &windowClass) {
    activeId = id;
    activeClass = windowClass;
    // Leaving the target cancels the operation even if focus later returns.
    if (operation != Operation::Idle && operation != Operation::Stopping && !targetId.isEmpty() && id != targetId) {
        targetLeft = true;
        if (keyRunning) keys.kill();
    }
}
QString StackController::State() const { return QStringLiteral("%1:%2").arg(int(operation)).arg(items.size()); }
void StackController::osd(const QString &text) {
    qInfo().noquote() << "stack-osd" << text;
    auto message = call(QStringLiteral("org.kde.plasmashell"), QStringLiteral("/org/kde/osdService"), QStringLiteral("org.kde.osdService"), QStringLiteral("showText"));
    message << QStringLiteral("edit-paste") << text;
    QDBusConnection::sessionBus().asyncCall(message, 500);
}
void StackController::load() {
    const QDir parent(QFileInfo(directory).absolutePath());
    for (const auto &name : parent.entryList({QStringLiteral("stack-cleared-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!QUuid(name.mid(14)).isNull() && !QFileInfo(parent.filePath(name)).isSymLink())
            QDir(parent.filePath(name)).removeRecursively(); // Finish a clear interrupted after its atomic rename.
    }
    const QDir dir(directory);
    const auto files = dir.entryList({QStringLiteral("*.qrs")}, QDir::Files, QDir::Name);
    if (files.size() > maxItems) { loadError = QStringLiteral("Stack storage invalid; clear it"); return; }
    for (qsizetype i = 0; i < files.size(); ++i) {
        if (files[i] != QStringLiteral("%1.qrs").arg(i, 3, 10, QLatin1Char('0'))) { loadError = QStringLiteral("Stack storage invalid; clear it"); break; }
        QFile file(dir.filePath(files[i]));
        Item item;
        if (!file.open(QIODevice::ReadOnly) || file.size() > maxBytes + 128 * 1024 || !decode(file.readAll(), item)) { loadError = QStringLiteral("Stack storage invalid; clear it"); break; }
        items << item;
    }
    if (!loadError.isEmpty()) items.clear();
}
bool StackController::available() {
    if (busy()) { osd(QStringLiteral("Stack busy")); return false; }
    if (!loadError.isEmpty()) { osd(loadError); return false; }
    if (!clipboard.ready()) { osd(QStringLiteral("Clipboard unavailable")); return false; }
    return true;
}
void StackController::append(const Item &item) {
    if (!loadError.isEmpty()) { osd(loadError); return; }
    if (items.size() >= maxItems) { osd(QStringLiteral("Stack full (100 items)")); return; }
    if (size(item) > maxBytes) { osd(QStringLiteral("Item exceeds 64 MB")); return; }
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) { osd(QStringLiteral("Cannot save stack")); return; }
    QSaveFile file(directory + QStringLiteral("/%1.qrs").arg(items.size(), 3, 10, QLatin1Char('0')));
    const auto bytes = encode(item);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) || file.write(bytes) != bytes.size() || !file.commit()) { osd(QStringLiteral("Cannot save stack")); return; }
    items << item;
    osd(items.size() == 1 ? QStringLiteral("Stack: 1 item") : QStringLiteral("Stack: %1 items").arg(items.size()));
}
void StackController::appendImage(const QImage &image) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) { osd(QStringLiteral("Cannot encode image")); return; }
    append({{QStringLiteral("image/png"), png}});
}
void StackController::clear() {
    if (busy()) { osd(QStringLiteral("Stack busy")); return; }
    // Rename is the commit point; a crash during deletion cannot resurrect a partially cleared stack.
    const QString trash = directory + QStringLiteral("-cleared-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (QDir(directory).exists() && !QDir().rename(directory, trash)) { osd(QStringLiteral("Cannot clear stack")); return; }
    items.clear();
    loadError.clear();
    if (QDir(trash).exists() && !QDir(trash).removeRecursively()) qWarning() << "stack-clear-cleanup-failed" << trash;
    osd(QStringLiteral("Stack cleared"));
}
bool StackController::sameTarget() const { return !targetLeft && (targetId.isEmpty() || activeId == targetId); }
void StackController::waitModifiers(std::function<void()> next) {
    targetId = activeId;
    targetLeft = false;
    targetTerminal = terminal(activeClass);
    afterModifiers = std::move(next);
    operation = Operation::Modifiers;
    modifierTime.start();
    modifiers.start();
}
void StackController::copy() {
    if (!available()) return;
    if (items.size() >= maxItems) { osd(QStringLiteral("Stack full (100 items)")); return; }
    waitModifiers([this] {
        operation = Operation::Copying;
        copyChanged = false;
        key(true, false, [this] {
            if (copyChanged) { operation = Operation::CopySnapshot; deadline.stop(); takeSnapshot(); }
        });
    });
}
void StackController::paste() {
    if (!available()) return;
    if (items.isEmpty()) { osd(QStringLiteral("Stack empty")); return; }
    waitModifiers([this] { operation = Operation::RestoreSnapshot; takeSnapshot(std::numeric_limits<qint64>::max()); });
}
void StackController::takeSnapshot(qint64 limit) {
    const quint64 id = ++serial;
    clipboard.snapshot(id, limit);
    QTimer::singleShot(700, this, [this, id] {
        if (id == serial && (operation == Operation::CopySnapshot || operation == Operation::RestoreSnapshot)) osd(QStringLiteral("Reading clipboard…"));
    });
}
void StackController::key(bool copy, bool image, std::function<void()> next) {
    if (physicalModifiers() != ModifierState::Released) {
        if (copy) finish(QStringLiteral("Release modifier keys"));
        else restore(QStringLiteral("Release modifier keys"));
        return;
    }
    const bool shift = targetTerminal && !image;
    QStringList args{QStringLiteral("key"), QStringLiteral("--key-delay=5"), QStringLiteral("29:1")};
    if (shift) args << QStringLiteral("42:1");
    args << (copy ? QStringLiteral("46:1") : QStringLiteral("47:1")) << (copy ? QStringLiteral("46:0") : QStringLiteral("47:0"));
    if (shift) args << QStringLiteral("42:0");
    args << QStringLiteral("29:0");
    qInfo().noquote() << "stack-key" << (copy ? "copy" : "paste") << "terminal=" << targetTerminal << "image=" << image;
    afterKeys = std::move(next);
    keyRunning = true;
    deadline.start(1200);
    keys.start(QStringLiteral("ydotool"), args);
}
void StackController::releaseSyntheticKeys() {
    if (releaseKeys.state() != QProcess::NotRunning) return;
    // ydotoold's virtual device outlives its CLI. A killed CLI must not leave Ctrl/Shift pressed on that device.
    releaseKeys.start(QStringLiteral("ydotool"), {QStringLiteral("key"), QStringLiteral("--key-delay=0"), QStringLiteral("46:0"), QStringLiteral("47:0"), QStringLiteral("42:0"), QStringLiteral("29:0")});
    QTimer::singleShot(1000, this, [this] { if (releaseKeys.state() != QProcess::NotRunning) releaseKeys.kill(); });
}
void StackController::nextPaste() {
    if (pasteIndex >= pasteItems.size()) { restore(QStringLiteral("Pasted %1 items").arg(items.size())); return; }
    if (!sameTarget() || locked()) { restore(QStringLiteral("Focus changed")); return; }
    auto item = pasteItems[pasteIndex];
    item.insert(secretHint, QByteArrayLiteral("secret"));
    operation = Operation::Offering;
    deadline.start(1500);
    clipboard.offer(item, ++serial);
}
void StackController::restore(const QString &message) {
    grace.stop(); deadline.stop();
    completion = message;
    operation = Operation::Restoring;
    clipboard.offer(original, ++serial);
}
void StackController::finish(const QString &message) {
    modifiers.stop(); deadline.stop(); grace.stop();
    operation = Operation::Idle;
    pasteItems.clear(); if (!restorePending) original.clear(); afterKeys = {}; afterModifiers = {};
    osd(message);
}
