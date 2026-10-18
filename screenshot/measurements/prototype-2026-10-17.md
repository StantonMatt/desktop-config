> Archived 2026-10-18: the prototype folder this describes was removed after its design became `quick-region-overlay.cpp`. Kept for its measurements.

# Quick region overlay prototype

This is an opt-in prototype for the Meta+Shift+S flow. It does not change the live launcher, shortcut, Spectacle configuration, or user service. The implementation is a resident Qt 6 / LayerShellQt C++ daemon; a small Rust `quick-region-overlay-trigger` binary is included only as a DBus trigger client. Qt was chosen for this prototype because LayerShellQt and the existing `KSystemClipboard` integration are already installed and avoid introducing a second Wayland protocol stack.

## Design

`quick-region-overlay-proto` pre-creates one `zwlr_layer_shell_v1` overlay window per `QScreen`. A DBus `Capture` call asks KWin's restricted `org.kde.KWin.ScreenShot2.CaptureWorkspace` for one raw native RGBA virtual-desktop image through a Unix pipe. The 7920x1440 buffer is wrapped directly as a `QImage` (no PNG decode), cropped per output, dimmed, and painted into the three surfaces. The windows use overlay-layer stacking, an exclusive keyboard grab, and a crosshair cursor.

A left-button press starts a desktop-coordinate drag. Motion updates only outputs intersecting the old/new selection, and the painter draws from the frozen image without allocating a full-output copy. Release with a selection crops native pixels and encodes only that region. When the resident `org.local.QuickRegion` service is available, the PNG is written to its existing `/run/user/$UID/quick-region-screenshot.png` inotify path; that service owns the clipboard and therefore keeps the existing Wayland/XWayland behavior. If the service is absent, the prototype falls back to `KSystemClipboard` directly. A click without movement leaves the overlay open. Escape closes all surfaces without changing the clipboard.

The installed authorization entry is [`quick-region-overlay-proto.desktop`](quick-region-overlay-proto.desktop). Its only purpose is KWin's `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` caller authorization; it has no shortcut and is not the live entry.

## Build and run

```sh
cd ~/Projects/desktop-config/screenshot/prototype-overlay
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
heavy cmake --build build
heavy cargo build --release
./run-prototype.sh
```

`run-prototype.sh` starts the resident daemon if needed and sends one `Capture` call. The manual DBus equivalent is:

```sh
gdbus call --session --dest org.local.QuickRegionOverlayProto \
  --object-path / --method org.local.QuickRegionOverlayProto.Capture
```

The daemon writes its optional test log and PID under `~/.cache/agent-scratch/desktop-config/`; stop that PID after testing. The prototype must not be installed as Meta+Shift+S until the user decides to switch. A future switch would need a new desktop entry/shortcut that invokes the trigger, a user service to keep the daemon resident, and removal of the temporary authorization entry only after the new executable has its final path.

## Measurements

The table uses the README method: injected input, KWin JavaScript `workspace.windowAdded` timestamps, daemon/journal timestamps, and Wayland `wl_surface.commit` tracing. The KWin script was loaded only for measurement and unloaded afterward. Prototype mapping trials each started a fresh daemon so the three layer surfaces were included in the press path; a production resident daemon amortizes process startup before the press.

| Stage | Prototype (3 trials, median) | Current Spectacle control (3 trials, median) |
| --- | ---: | ---: |
| Key/DBus request → first overlay mapped | **99 ms** (96–102) | **258 ms** (241–269) |
| Key/DBus request → all three overlays mapped | **100 ms** (96–103) | **387 ms** (376–412) |
| Drag frame interval on DP-5, median | **5.1 ms** | **5.7 ms** |
| Drag frame interval on DP-5, p95 | **7.8 ms** | **10.5 ms** |
| Release → clipboard API update | **2 ms** median (1–12) | **11 ms** fresh same-session control |
| Release → image readable by native Wayland client | **about 43 ms** in the final smoke readback | **42 ms** fresh same-session control |

The prototype's KWin capture/read floor was 73 ms median (three direct captures: 73, 73, 77 ms; the same raw payload is 45,619,200 bytes). Recent system load produced 97–111 ms captures, so the floor is the hard limit for this `CaptureWorkspace` approach. The mapping overhead in the measured trials was about 20 ms after the raw buffer arrived, putting the all-monitor median at the 100 ms target boundary.

A fresh Spectacle control was injected through the unchanged Meta+Shift+S shortcut in the same session. The three first/all mapping pairs were 269/412, 258/387, and 241/376 ms. The drag control used the same Spectacle binary with `QT_QPA_PLATFORM=wayland` and `QSG_RENDER_LOOP=threaded`, while tracing its DP-5 commits.

Clipboard checks used `wl-paste` plus a visible `QT_QPA_PLATFORM=xcb` Qt receiver. The receiver read a fresh 601x301 image in both cases. A click-only trial logged `click-no-capture` and left the overlay until Escape; Escape logged `escape-cancel` and made no clipboard write. A cross-output drag from HDMI-A-5 into DP-5 produced a 3001x101 crop and closed all three surfaces.

## Missing versus Spectacle

The prototype intentionally omits Spectacle's magnifier, annotations, toolbar/editor, delay timer, window/area modes, notifications, and format controls. It captures the whole workspace once rather than starting a full QML editor, so the frozen image can be a few milliseconds after the key press. The prototype currently assumes the monitor layout remains stable during one capture and does not persist a screenshot after both the prototype and existing clipboard owner exit.

## Files and cleanup

Files in this folder are the CMake project, C++ daemon, Rust trigger, run script, desktop authorization entry, and this README. The only file outside the folder created for authorization is:

- `~/.local/share/applications/quick-region-overlay-proto.desktop` (same content as the checked-in copy; remove it to revoke ScreenShot2 authorization).

No packages were needed for the prototype build. `xclip` was installed only for an additional X11 probe; the acceptance check used the already-installed PyQt6 XWayland receiver. No prototype daemon, test overlay, KWin measurement script, or input injector is left running. The live `quick-region-clipboard.service` and its idle Spectacle are the user's existing resident processes and were not stopped or modified.
