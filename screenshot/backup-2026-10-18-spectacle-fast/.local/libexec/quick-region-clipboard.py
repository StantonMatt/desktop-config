#!/usr/bin/python3

import os
import signal
import sys

from PyQt6.QtCore import QTimer
from PyQt6.QtGui import QGuiApplication, QImage


def main() -> int:
    if len(sys.argv) != 2:
        return 2

    image_path = sys.argv[1]
    image = QImage(image_path)
    if image.isNull():
        return 1

    app = QGuiApplication([sys.argv[0]])
    app.setApplicationName("Quick Region Clipboard")
    clipboard = app.clipboard()

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda _signum, _frame: app.quit())

    clipboard.setImage(image)
    try:
        os.unlink(image_path)
    except FileNotFoundError:
        pass

    armed = False

    def arm_exit() -> None:
        nonlocal armed
        armed = True

    def exit_when_replaced() -> None:
        if armed:
            app.quit()

    clipboard.dataChanged.connect(exit_when_replaced)
    QTimer.singleShot(1000, arm_exit)
    signal_timer = QTimer()
    signal_timer.timeout.connect(lambda: None)
    signal_timer.start(250)
    QTimer.singleShot(12 * 60 * 60 * 1000, app.quit)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
