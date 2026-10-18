#!/usr/bin/python3

import os
import signal
import subprocess
import sys

from PyQt6.QtCore import QCoreApplication, Qt, QTimer
from PyQt6.QtGui import QGuiApplication, QImage


CAPTURE_PATH = f"/run/user/{os.getuid()}/quick-region-screenshot.png"
SPECTACLE_DESKTOP_FILE = (
    "/home/mjstanton/.local/share/applications/org.kde.spectacle.desktop"
)


def main() -> int:
    if "-session" in sys.argv:
        print("ignoring a Plasma session-restore launch", flush=True)
        return 0

    QCoreApplication.setAttribute(
        Qt.ApplicationAttribute.AA_DisableSessionManager
    )
    app = QGuiApplication([sys.argv[0]])
    app.setApplicationName("Quick Region Clipboard")
    clipboard = app.clipboard()
    print(
        f"clipboard service started: platform={app.platformName()} "
        f"display={os.environ.get('DISPLAY', '')}",
        flush=True,
    )
    current_image = None
    last_signature = None
    retry_in_progress = False

    def reset_retry_guard() -> None:
        nonlocal retry_in_progress
        retry_in_progress = False

    def reopen_region_selection() -> None:
        subprocess.Popen(
            ["/usr/bin/kioclient", "exec", SPECTACLE_DESKTOP_FILE],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        print("reopened region selection after empty selection", flush=True)

    def check_for_capture() -> None:
        nonlocal current_image, last_signature, retry_in_progress

        try:
            stat = os.stat(CAPTURE_PATH)
        except FileNotFoundError:
            last_signature = None
            return

        signature = (stat.st_size, stat.st_mtime_ns)
        if signature != last_signature:
            last_signature = signature
            return

        image = QImage(CAPTURE_PATH)
        if image.isNull():
            print("capture file is not yet a readable image", flush=True)
            return

        try:
            os.unlink(CAPTURE_PATH)
        except FileNotFoundError:
            return

        virtual_size = app.primaryScreen().virtualGeometry().size()
        if image.size() == virtual_size:
            print(
                f"discarded empty-selection fallback: "
                f"{image.width()}x{image.height()}",
                flush=True,
            )
            last_signature = None
            if not retry_in_progress:
                retry_in_progress = True
                QTimer.singleShot(750, reopen_region_selection)
                QTimer.singleShot(60_000, reset_retry_guard)
            return

        retry_in_progress = False
        current_image = image
        last_signature = None
        clipboard.setImage(current_image)
        print(
            f"copied capture: {image.width()}x{image.height()} "
            f"owns_clipboard={clipboard.ownsClipboard()}",
            flush=True,
        )

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda _signum, _frame: app.quit())

    watcher = QTimer()
    watcher.timeout.connect(check_for_capture)
    watcher.start(100)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
