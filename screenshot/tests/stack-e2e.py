#!/usr/bin/python3
"""Isolated KWin/Wayland integration check. Run through heavy, never on the live display.

Usage: heavy python3 tests/stack-e2e.py /absolute/build/path
The outer process starts a private D-Bus session; all XDG state, offers and input stay there.
"""
import base64
import contextlib
import json
import os
import pathlib
import subprocess
import signal
import sys
import time
import tempfile
import shutil

HERE = pathlib.Path(__file__).resolve().parent
BUILD = pathlib.Path(sys.argv[1]).resolve()
OUT = pathlib.Path.home() / '.cache/agent-scratch/desktop-config/clipboard-stack/e2e'
OUT.mkdir(parents=True, exist_ok=True)
TERMINAL_ONLY = os.environ.get('QRO_TERMINAL_ONLY') == '1'
runtime = pathlib.Path(os.environ.get('QRO_STACK_RUNTIME') or tempfile.mkdtemp(prefix='qro-stack-'))
os.environ['QRO_STACK_RUNTIME'] = str(runtime)
runtime.mkdir(exist_ok=True, mode=0o700)
os.chmod(runtime, 0o700)
os.environ.update(XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY='qro-stack-test-0',
                  XDG_CONFIG_HOME=str(OUT / 'config'), XDG_DATA_HOME=str(OUT / 'data'),
                  XDG_STATE_HOME=str(OUT / 'state'), XDG_CACHE_HOME=str(OUT / 'cache'),
                  QT_QPA_PLATFORM='wayland', KWIN_COMPOSE='O2', QT_QUICK_BACKEND='software',
                  XDG_CURRENT_DESKTOP='KDE', KDE_FULL_SESSION='true')
os.environ.pop('DISPLAY', None)
os.environ.pop('SESSION_MANAGER', None)
os.environ.pop('DBUS_STARTER_ADDRESS', None)
os.environ.pop('DBUS_STARTER_BUS_TYPE', None)
os.environ.pop('QT_WAYLAND_RECONNECT', None)
if '--inner' not in sys.argv:
    child = subprocess.Popen(['dbus-run-session', '--', sys.executable, str(__file__), str(BUILD), '--inner'], start_new_session=True)
    try: result = child.wait()
    finally:
        # Includes any portal/desktop service D-Bus activated in this test's private process group.
        try: os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError: pass
        shutil.rmtree(runtime, ignore_errors=True)
    sys.exit(result)
(OUT / 'bus-address').write_text(os.environ['DBUS_SESSION_BUS_ADDRESS'])
os.environ['QT_LOGGING_RULES'] = 'org.kde.klipper.debug=true;kf.guiaddons.debug=true'
bin_dir = OUT / 'bin'
bin_dir.mkdir(exist_ok=True)
shim = bin_dir / 'ydotool'
shim.write_text('#!/bin/sh\nexec "' + str(BUILD / 'stack-input') + '" "$@"\n')
shim.chmod(0o700)
os.environ['PATH'] = str(bin_dir) + ':' + os.environ['PATH']
apps = OUT / 'data/applications'
apps.mkdir(parents=True, exist_ok=True)
(apps / 'quick-region-overlay.desktop').write_text('[Desktop Entry]\nType=Application\nName=Quick Region Overlay\nExec=' + str(BUILD / 'quick-region-overlay') + ' --capture\nNoDisplay=true\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n')
(apps / 'qro-stack-input.desktop').write_text('[Desktop Entry]\nType=Application\nName=QRO isolated input test\nExec=' + str(BUILD / 'stack-input') + '\nNoDisplay=true\nX-KDE-Wayland-Interfaces=org_kde_kwin_fake_input\n')
config = OUT / 'config'
config.mkdir(exist_ok=True)
(config / 'kglobalshortcutsrc').write_text('[services][org.kde.spectacle.desktop]\n_launch=none\n')
(config / 'klipperrc').write_text('[General]\nMaxClipItems=40\nKeepClipboardContents=true\nIgnoreImages=false\n')
subprocess.run(['kbuildsycoca6', '--noincremental'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

def run(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True, timeout=15).stdout.strip()

def dbus(service, path, method, *args, check=True):
    return run('qdbus6', service, path, method, *map(str, args), check=check)

def probe(method, *args):
    return dbus('org.local.StackProbe', '/', 'org.local.StackProbe.' + method, *args)

def stack(method):
    return dbus('org.local.QuickRegionOverlay', '/Stack', 'org.local.QuickRegionOverlay.Stack.' + method)

def wait(predicate, description, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            result = predicate()
            if result: return result
        except (subprocess.CalledProcessError, ValueError, json.JSONDecodeError): pass
        time.sleep(0.05)
    raise RuntimeError('Timed out: ' + description)

def stop(process):
    if process.poll() is not None: return
    process.terminate()
    try: process.wait(timeout=6)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait(timeout=5)

def input_keys(*keys):
    run(str(BUILD / 'stack-input'), *keys)

def shortcut(letter):
    assert stack('Focus') == 'qro-stack-probe', 'test editor not focused'
    code = {'c': 46, 's': 31, 'v': 47, 'x': 45}[letter]
    input_keys('29:1', '56:1', f'{code}:1', f'{code}:0', '56:0', '29:0')

def clipboard():
    probe('ReadClipboard')
    result = wait(lambda: json.loads(probe('Clipboard')) or None, 'clipboard snapshot', timeout=70)
    error = result.pop('_error')
    assert error == '', error
    return {key: base64.b64decode(value) for key, value in result.items()}

def history():
    result = subprocess.run(['qdbus6', 'org.kde.klipper', '/klipper', 'org.kde.klipper.klipper.getClipboardHistoryMenu'], capture_output=True, text=True, timeout=10)
    if result.returncode:
        (OUT / 'history-error').write_text(result.stderr + '\n' + result.stdout)
    return result.stdout.strip()

def copy(text, expected):
    probe('Prepare', text)
    if probe('FocusReady') != 'true':
        info = dbus('org.kde.KWin', '/KWin', 'org.kde.KWin.getWindowInfo', stack('FocusId'))
        geometry = dict(line.split(': ', 1) for line in info.splitlines() if ': ' in line)
        x, y = int(float(geometry['x'])) + 100, int(float(geometry['y'])) + 100
        run(str(BUILD / 'stack-input'), 'drag', str(x), str(y), str(x), str(y))
        probe('Prepare', text)
        wait(lambda: probe('FocusReady') == 'true', 'test editor keyboard focus')
    shortcut('c')
    wait(lambda: stack('Count') == str(expected), f'copy {expected}')

results = []
def passed(name, **evidence):
    results.append(dict(check=name, **evidence))
    (OUT / ('results-terminal.json' if TERMINAL_ONLY else 'results.json')).write_text(json.dumps(results, indent=2))
    print(json.dumps({key: value for key, value in results[-1].items() if key != 'events'}), flush=True)

with contextlib.ExitStack() as cleanup:
    def start(name, args):
        log = cleanup.enter_context((OUT / (name + '.log')).open('a'))
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        cleanup.callback(stop, process)
        print(name, 'PID', process.pid, flush=True)
        return process
    kwin = start('kwin', ['kwin_wayland', '--virtual', '--width', '1024', '--height', '768', '--socket', 'qro-stack-test-0', '--no-lockscreen', '--no-kactivities'])
    wait(lambda: (runtime / 'qro-stack-test-0').exists() and dbus('org.kde.KWin', '/KWin', 'org.kde.KWin.activeOutputName'), 'virtual KWin')
    keyboard = start('input-keeper', [str(BUILD / 'stack-input'), '--keepalive'])
    shell = start('klipper', [str(BUILD / 'stack-klipper')])
    wait(lambda: dbus('org.kde.klipper', '/klipper', 'org.freedesktop.DBus.Peer.Ping') == '', 'Klipper', timeout=20)
    (OUT / 'klipper-methods').write_text(run('qdbus6', 'org.kde.klipper', '/klipper', check=False))
    daemon = start('daemon', [str(BUILD / 'quick-region-overlay'), '--daemon'])
    wait(lambda: stack('State'), 'daemon')
    editor = start('probe', [str(BUILD / 'stack-probe')])
    wait(lambda: stack('Focus') == 'qro-stack-probe', 'editor focus')
    # Existing isolated data from an interrupted run belongs to this harness.
    shortcut('x'); wait(lambda: stack('Count') == '0', 'initial clear')
    if not TERMINAL_ONLY:
        copy('alpha', 1)
        assert clipboard()['text/plain'] == b'alpha'
        probe('ResetEvents')
        input_keys('29:1', '47:1', '47:0', '29:0')
        first = json.loads(probe('Events'))
        assert base64.b64decode(first[0]['text/plain']) == b'alpha'
        copy('alpha', 2)
        passed('GUI copy, identical copies, normal paste', count=2)
        shortcut('x'); wait(lambda: stack('Count') == '0', 'clear')
        copy('one', 1); copy('two', 2)
        probe('Custom', 'image', '#ff0000'); shortcut('c'); wait(lambda: stack('Count') == '3', 'copy image')
        copy('three', 4)
        probe('Custom', 'binary', 'restore sentinel'); probe('Offer')
        time.sleep(0.2)
        wait(lambda: 'restore sentinel' in history(), 'Klipper observes ordinary copy', timeout=5)
        history_before = history()
        before = clipboard()
        probe('ResetEvents'); shortcut('v')
        wait(lambda: stack('State') == '0:4', 'stack paste')
        events = json.loads(probe('Events'))
        assert len(events) == 3, events
        assert base64.b64decode(events[0]['text/plain']) == b'one\ntwo'
        assert 'image/png' in events[1]
        assert base64.b64decode(events[2]['text/plain']) == b'three'
        after = clipboard()
        assert after == before, (after, before)
        assert history() == history_before, (history(), history_before)
        passed('Klipper excludes intermediate offers', before=history_before, after=history())
        passed('text,text,image,text order and exact clipboard restore', events=events)
        probe('PasteDelay', 250); probe('ResetEvents'); shortcut('v')
        wait(lambda: len(json.loads(probe('Events'))) == 3, 'delayed target reads')
        wait(lambda: stack('State') == '0:4', 'delayed paste completion')
        delayed = json.loads(probe('Events'))
        assert base64.b64decode(delayed[0]['text/plain']) == b'one\ntwo'
        assert 'image/png' in delayed[1]
        assert base64.b64decode(delayed[2]['text/plain']) == b'three'
        probe('PasteDelay', 0); probe('IgnorePaste', 'true'); probe('ResetEvents'); shortcut('v')
        wait(lambda: stack('State').startswith('6:'), 'target offered item')
        wait(lambda: stack('State') == '0:4', 'no-read timeout')
        assert json.loads(probe('Events')) == []
        assert clipboard() == before
        probe('IgnorePaste', 'false')
        passed('read-driven pacing survives 250 ms target delay; no-read timeout restores clipboard')
        stop(daemon)
        daemon = start('daemon-restart', [str(BUILD / 'quick-region-overlay'), '--daemon'])
        wait(lambda: stack('State') == '0:4', 'persisted stack')
        passed('daemon restart preserves stack', count=4)
        shortcut('x'); wait(lambda: stack('Count') == '0', 'clear persisted stack')
        stop(daemon)
        daemon = start('daemon-cleared', [str(BUILD / 'quick-region-overlay'), '--daemon'])
        wait(lambda: stack('State') == '0:0', 'clear survives restart')
        passed('clear persists', count=0)
        passed('shortcuts', capture=dbus('org.local.QuickRegionOverlay', '/', 'org.local.QuickRegionOverlay.ShortcutStatus'), stack=dbus('org.local.QuickRegionOverlay', '/', 'org.local.QuickRegionOverlay.StackShortcutStatus'))
        # Screenshot variants: a real crop offer, and no append through the two original shortcuts.
        shortcut('s'); time.sleep(0.3)
        run(str(BUILD / 'stack-input'), 'drag', '300', '300', '420', '380')
        wait(lambda: stack('Count') == '1', 'stack screenshot')
        assert 'image/png' in clipboard()
        for sequence in [('99:1', '99:0'), ('125:1', '42:1', '31:1', '31:0', '42:0', '125:0')]:
            assert stack('Focus') == 'qro-stack-probe'
            input_keys(*sequence); time.sleep(0.3)
            run(str(BUILD / 'stack-input'), 'drag', '320', '320', '440', '400')
            time.sleep(0.2)
            assert stack('Count') == '1'
            assert 'image/png' in clipboard()
        passed('Ctrl+Alt+S appends; Print and Meta+Shift+S retain normal capture behavior')
        shortcut('x'); wait(lambda: stack('Count') == '0', 'clear before Konsole')
        for count in range(1, 101): copy('cap test', count)
        stack_dir = OUT / 'state/quick-region-overlay/stack'
        assert len(list(stack_dir.glob('*.qrs'))) == 100
        assert stack_dir.stat().st_mode & 0o777 == 0o700
        assert all(path.stat().st_mode & 0o777 == 0o600 for path in stack_dir.glob('*.qrs'))
        probe('Prepare', 'overflow refused'); shortcut('c')
        time.sleep(0.1)
        assert stack('Count') == '100'
        passed('100-item cap refuses overflow; exact state path and private permissions')
        shortcut('x'); wait(lambda: stack('Count') == '0', 'clear capped stack')
    terminal_file = OUT / 'terminal-input'
    terminal_file.write_bytes(b'')
    terminal_file.with_suffix('.ready').unlink(missing_ok=True)
    console = start('konsole', ['konsole', '--separate', '--nofork', '--hide-menubar', '--hide-tabbar', '-e', sys.executable, str(HERE / 'stack-terminal.py'), str(terminal_file)])
    wait(lambda: stack('Focus') in ('konsole', 'org.kde.konsole'), 'Konsole focus')
    wait(lambda: terminal_file.with_suffix('.ready').exists(), 'Konsole child output')
    console_service = 'org.kde.konsole-' + str(console.pid)
    wait(lambda: 'konsole-selected' in dbus(console_service, '/Sessions/1', 'org.kde.konsole.Session.getAllDisplayedText', 'true'), 'Konsole displays child output')
    (OUT / 'konsole-dbus-paths').write_text(run('qdbus6', 'org.kde.konsole-' + str(console.pid), check=False))
    # Select the printed line with the mouse. Query KWin, since Wayland QWidget geometry has no global position.
    info = dbus('org.kde.KWin', '/KWin', 'org.kde.KWin.getWindowInfo', stack('FocusId'))
    geometry = dict(line.split(': ', 1) for line in info.splitlines() if ': ' in line)
    x, y = int(float(geometry['x'])), int(float(geometry['y']))
    run(str(BUILD / 'stack-input'), 'drag', str(x + 6), str(y + 40), str(x + 200), str(y + 40))
    try:
        input_keys('29:1', '42:1', '46:1', '46:0', '42:0', '29:0')
        selected = clipboard()
        assert selected.get('text/plain', selected.get('text/plain;charset=utf-8', b'')).strip() == b'konsole-selected', 'Konsole selection setup failed'
        run(str(BUILD / 'stack-input'), 'drag', str(x + 6), str(y + 40), str(x + 200), str(y + 40))
        input_keys('29:1', '56:1', '46:1', '46:0', '56:0', '29:0')
        wait(lambda: stack('Count') == '1', 'Konsole copy')
    except (RuntimeError, AssertionError):
        for path in ['/Sessions/1', '/Windows/1', '/konsole/MainWindow_1', '/konsole/MainWindow_1/actions']:
            (OUT / ('konsole-' + path.replace('/', '_') + '.methods')).write_text(run('qdbus6', console_service, path, check=False))
        run('spectacle', '-b', '-n', '-f', '-o', str(OUT / 'konsole-selection.png'), check=False)
        raise
    term_copy = clipboard()
    assert term_copy.get('text/plain', term_copy.get('text/plain;charset=utf-8', b'')).strip() == b'konsole-selected', term_copy
    assert terminal_file.read_bytes() == b'', terminal_file.read_bytes()
    input_keys('29:1', '56:1', '47:1', '47:0', '56:0', '29:0')
    wait(lambda: stack('State') == '0:1', 'Konsole paste')
    wait(lambda: b'konsole-selected' in terminal_file.read_bytes(), 'terminal receives text')
    passed('Konsole Ctrl+Shift+C/V, no interrupt', received=terminal_file.read_bytes().decode(errors='replace'))
    stop(console)
    wait(lambda: stack('Focus') == 'qro-stack-probe', 'editor focus after Konsole')
    shortcut('x'); wait(lambda: stack('Count') == '0', 'clear terminal selection')
    copy('terminal receiver text', 1)
    probe('Custom', 'image', '#0080ff'); shortcut('c'); wait(lambda: stack('Count') == '2', 'image for terminal receiver')
    probe('Quit'); editor.wait(timeout=5)
    editor = start('terminal-probe', [str(BUILD / 'stack-probe'), '--terminal'])
    wait(lambda: stack('Focus') in ('konsole', 'org.kde.konsole'), 'terminal-class MIME receiver')
    probe('Custom', 'binary', 'terminal restore'); probe('Offer'); time.sleep(0.1)
    input_keys('29:1', '56:1', '47:1', '47:0', '56:0', '29:0')
    wait(lambda: len(json.loads(probe('Events'))) == 2, 'terminal text/image paste')
    wait(lambda: stack('State') == '0:2', 'terminal image paste completion')
    routed = json.loads(probe('Events'))
    assert routed[0]['_key'] == 'Ctrl+Shift+V' and base64.b64decode(routed[0]['text/plain']) == b'terminal receiver text'
    assert routed[1]['_key'] == 'Ctrl+V' and 'image/png' in routed[1]
    passed('terminal text uses Ctrl+Shift+V; terminal image uses Ctrl+V', events=routed)
    input_keys('29:1', '56:1', '45:1', '45:0', '56:0', '29:0')
    wait(lambda: stack('Count') == '0', 'final clear')
print('PASS; logs and results:', OUT, flush=True)
