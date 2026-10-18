// Pure daemon decisions shared by quick-region-overlay.cpp and tests/daemon-logic-test.cpp: command-line handling,
// the ShortcutStatus answer and the capture state machine.
#pragma once
#include <QByteArray>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <chrono>
#include <optional>

namespace DaemonLogic {

// Only quick-region-overlay.service runs --daemon; --capture asks it for a capture over D-Bus.
enum class Command { Capture, Daemon, Usage };
inline Command parseCommand(const QStringList &args) {
    if (args == QStringList{QStringLiteral("--capture")}) return Command::Capture;
    if (args == QStringList{QStringLiteral("--daemon")}) return Command::Daemon;
    return Command::Usage;
}
// Exit status for a usage error, and for "another daemon owns the name", which the unit's RestartPreventExitStatus
// keeps systemd from looping on.
inline constexpr int usageStatus = 2;
inline constexpr int alreadyRunningStatus = 3;
// The --daemon exit status when the D-Bus name is already owned, or nothing to continue.
inline std::optional<int> alreadyRunning(bool nameOwned) { return nameOwned ? std::optional<int>(alreadyRunningStatus) : std::nullopt; }

// One sequence from KGlobalAccel's shortcutKeys, which sends each as its four combined chords, unused ones 0.
inline QKeySequence keySequence(const QList<int> &chords) {
    int keys[4] = {};
    for (qsizetype i = 0; i < qMin<qsizetype>(chords.size(), 4); ++i) keys[i] = chords[i];
    return QKeySequence(keys[0], keys[1], keys[2], keys[3]);
}

// The keys that bind anything: KGlobalAccel keeps an empty sequence for a cleared key or a default it couldn't grant.
inline QList<QKeySequence> activeKeys(QList<QKeySequence> keys) {
    keys.removeAll(QKeySequence());
    return keys;
}

// ShortcutStatus: "registered:<keys>" while the action has keys, otherwise "none", or "none:<component/action>, ..."
// naming who holds the default key. Sequences are separated by "; " and the chords of one by ", ", so the result
// remains machine-parseable.
inline QString shortcutStatus(const QList<QKeySequence> &liveKeys, const QStringList &defaultKeyOwners) {
    const QList<QKeySequence> keys = activeKeys(liveKeys);
    if (!keys.isEmpty()) return QStringLiteral("registered:") + QKeySequence::listToString(keys);
    if (defaultKeyOwners.isEmpty()) return QStringLiteral("none");
    return QStringLiteral("none:") + defaultKeyOwners.join(QStringLiteral(", "));
}

// One press's way to an overlay. A press starts a capture; a layout change aborts it (the serial goes up, so KWin's
// late reply is discarded) and the press captures again after the windows are rebuilt, however often the layout
// changes, until giveUp has passed since the press. A reply whose shape doesn't fit an unchanged layout is retried
// once per press; the second one fails the capture.
class CaptureFlow {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds giveUp{2000};
    enum class AfterRebuild { Nothing, Retry, Dropped };
    // What a shortcut press does: capture when idle, close an open overlay (so a stuck one can always be dismissed),
    // and nothing while a capture is in flight or waiting for a rebuild.
    enum class Press { Capture, Cancel, Busy };

    // A capture in flight, an overlay open or a press waiting for a rebuild.
    bool busy() const { return capturing || shown || retryPending; }
    bool overlayShown() const { return shown; }
    Press shortcutPress() const {
        if (shown) return Press::Cancel;
        return busy() ? Press::Busy : Press::Capture;
    }
    void pressed(Clock::time_point now) { pressedAt = now; shapeRetries = 0; }
    // The windows are being rebuilt; capture once they are.
    void defer() { retryPending = true; }
    quint64 begin() { capturing = true; return ++serial; }
    bool isCurrent(quint64 replySerial) const { return capturing && replySerial == serial; }
    // The layout changed: any reply in flight is stale. Returns whether that aborted a capture, which then retries.
    bool layoutChanged() {
        ++serial;
        if (!capturing) return false;
        capturing = false;
        retryPending = true;
        return true;
    }
    // The reply was for another layout; capture again after the rebuild the caller schedules.
    void retryAfterRebuild() { capturing = false; retryPending = true; }
    // The reply's shape doesn't fit the layout Qt reports. The first time in a press, retry after a rebuild like
    // retryAfterRebuild; the second time returns false and the caller fails the capture.
    bool retryShape() {
        if (++shapeRetries > 1) return false;
        retryAfterRebuild();
        return true;
    }
    AfterRebuild rebuilt(Clock::time_point now) {
        if (!retryPending) return AfterRebuild::Nothing;
        retryPending = false;
        if (!pressedAt || now - *pressedAt > giveUp) { pressedAt.reset(); return AfterRebuild::Dropped; }
        return AfterRebuild::Retry;
    }
    void captured() { capturing = false; shown = true; pressedAt.reset(); }
    void failed() { capturing = false; pressedAt.reset(); }
    void closed() { shown = false; }
private:
    bool capturing = false;
    bool shown = false;
    bool retryPending = false;
    quint64 serial = 0;
    int shapeRetries = 0;
    std::optional<Clock::time_point> pressedAt;
};

} // namespace DaemonLogic
