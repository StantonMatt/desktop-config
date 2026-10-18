// ScreenShot2 capture-path benchmark: workspace vs per-screen captures over a pipe, a 1 MiB pipe or a memfd.
// Usage: quick-region-capture-bench <ws-pipe|ws-bigpipe|ws-memfd|sc-pipe|sc-bigpipe|sc-memfd> <trials> [nohide]
// KWin authorizes ScreenShot2 by executable path: install the binary as ~/.local/libexec/quick-region-capture-bench with a
// matching quick-region-capture-bench.desktop (X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2), rebuild the
// sycoca cache, and remove both afterwards. Times are ms from the request: reply received / all pixels readable.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct Job { QString method; QString screen; int kind; int rfd = -1; int wfd = -1; qint64 reply = -1; qint64 done = -1; qint64 bytes = 0; std::thread th; };
static QElapsedTimer T;
static bool readFully(int fd, char *p, qint64 n) { while (n > 0) { ssize_t r = ::read(fd, p, n); if (r < 0 && errno == EINTR) continue; if (r <= 0) return false; p += r; n -= r; } return true; }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: %s <ws-pipe|ws-bigpipe|ws-memfd|sc-pipe|sc-bigpipe|sc-memfd> <trials> [nohide]\n", argv[0]); return 2; }
    const QString mode = argv[1]; const int trials = atoi(argv[2]);
    const bool hideFalse = argc > 3 && QString(argv[3]) == "nohide";
    // mode: ws-pipe ws-bigpipe ws-memfd sc-pipe sc-bigpipe sc-memfd
    const bool screens = mode.startsWith("sc");
    const int kind = mode.endsWith("memfd") ? 2 : mode.endsWith("bigpipe") ? 1 : 0;
    int trial = 0;
    std::vector<Job *> jobs;
    std::function<void()> run;
    auto finishTrial = [&] {
        for (auto *j : jobs) if (j->th.joinable()) j->th.join();
        qint64 firstDone = 1e9, lastDone = 0, lastReply = 0, firstReply = 1e9;
        QString detail;
        for (auto *j : jobs) { firstDone = qMin(firstDone, j->done); lastDone = qMax(lastDone, j->done); firstReply = qMin(firstReply, j->reply); lastReply = qMax(lastReply, j->reply); detail += QString(" %1:%2/%3").arg(j->screen.isEmpty() ? "ws" : j->screen).arg(j->reply / 1000.0, 0, 'f', 1).arg(j->done / 1000.0, 0, 'f', 1); }
        printf("%s%s trial=%d first_reply=%.1f last_reply=%.1f first_done=%.1f last_done=%.1f%s\n", qPrintable(mode), hideFalse ? "-nohide" : "", trial, firstReply / 1000.0, lastReply / 1000.0, firstDone / 1000.0, lastDone / 1000.0, qPrintable(detail));
        fflush(stdout);
        for (auto *j : jobs) delete j; jobs.clear();
        if (++trial < trials) QTimer::singleShot(400, run); else app.quit();
    };
    run = [&] {
        QStringList names = screens ? QStringList{"HDMI-A-5", "DP-5", "DP-4"} : QStringList{QString()};
        auto pending = std::make_shared<std::atomic<int>>(names.size());
        T.restart();
        for (const auto &name : names) {
            auto *j = new Job; j->screen = name; jobs.push_back(j);
            if (kind == 2) { j->wfd = memfd_create("qro", MFD_CLOEXEC); j->rfd = j->wfd; }
            else { int p[2]; pipe2(p, O_CLOEXEC); j->rfd = p[0]; j->wfd = p[1]; if (kind == 1) fcntl(p[1], F_SETPIPE_SZ, 1 << 20); }
            QVariantMap options; options.insert("include-cursor", false); options.insert("native-resolution", true);
            if (hideFalse) options.insert("hide-caller-windows", false);
            auto msg = name.isEmpty() ? QDBusMessage::createMethodCall("org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2", "CaptureWorkspace")
                                      : QDBusMessage::createMethodCall("org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2", "CaptureScreen");
            if (!name.isEmpty()) msg << name;
            msg << options << QVariant::fromValue(QDBusUnixFileDescriptor(j->wfd));
            auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg));
            if (kind != 2) ::close(j->wfd);
            QObject::connect(w, &QDBusPendingCallWatcher::finished, [&, j, w, pending] {
                QDBusPendingReply<QVariantMap> r = *w; w->deleteLater();
                j->reply = T.nsecsElapsed() / 1000;
                if (r.isError()) { fprintf(stderr, "error %s\n", qPrintable(r.error().message())); exit(1); }
                const auto m = r.value();
                const qint64 size = qint64(m.value("stride").toInt()) * m.value("height").toInt();
                j->bytes = size;
                if (kind == 2) {
                    // KWin writes after replying; poll the memfd size until the whole image is there, for at most 2 s.
                    struct stat st;
                    for (;;) {
                        if (fstat(j->rfd, &st) == 0 && st.st_size >= size) break;
                        if (T.nsecsElapsed() / 1000000 - j->reply / 1000 > 2000) { fprintf(stderr, "timeout waiting for %lld bytes\n", (long long)size); exit(1); }
                        usleep(200);
                    }
                    void *p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, j->rfd, 0);
                    QImage img((const uchar *)p, m.value("width").toInt(), m.value("height").toInt(), m.value("stride").toInt(), QImage::Format(m.value("format").toUInt()));
                    j->done = T.nsecsElapsed() / 1000;
                    munmap(p, size); ::close(j->rfd);
                    if (--*pending == 0) finishTrial();
                } else {
                    j->th = std::thread([j, size, pending, &finishTrial] {
                        QImage img(1, 1, QImage::Format_ARGB32);
                        std::vector<char> buf(size);
                        readFully(j->rfd, buf.data(), size); ::close(j->rfd);
                        j->done = T.nsecsElapsed() / 1000;
                        if (--*pending == 0) QMetaObject::invokeMethod(qApp, [&finishTrial] { finishTrial(); }, Qt::QueuedConnection);
                    });
                }
            });
        }
    };
    QTimer::singleShot(0, run);
    return app.exec();
}
