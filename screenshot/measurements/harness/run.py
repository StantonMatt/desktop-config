#!/usr/bin/python3 -I
# User-side test controller. Usage: run.py <scenario> [trials]
# Build the clipboard probe first: cmake -S . -B ~/.cache/agent-scratch/desktop-config/probe-build && cmake --build ~/.cache/agent-scratch/desktop-config/probe-build
# The injector runs through sudo -n and creates its uinput devices only for the duration of a run; the run's cleanup
# (the ExitStack below) stops it, the clipboard probe and the KWin script, and puts DP-4 back, even after an error.
import contextlib, json, os, select, subprocess, sys, time
H = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = {'measure', 'behaviour', 'magnifier', 'latency', 'gap', 'busygap', 'presslag', 'magtext', 'layout', 'directions',
             'shiftheld', 'secondlaunch', 'press', 'spectacle', 'print', 'printclick'}
# Reject bad arguments before anything is set up.
if len(sys.argv) not in (2, 3) or sys.argv[1] not in SCENARIOS:
    sys.exit(f'usage: run.py <scenario> [trials]\nscenarios: {" ".join(sorted(SCENARIOS))}')
scenario = sys.argv[1]
try: trials = int(sys.argv[2]) if len(sys.argv) > 2 else 1
except ValueError: trials = 0
if trials < 1: sys.exit('trials must be a whole number of at least 1')
OUT = os.path.expanduser('~/.cache/agent-scratch/desktop-config/harness-out'); os.makedirs(OUT, exist_ok=True)
now = lambda: int(time.time() * 1000)
start_wall = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(time.time() - 1))
pastelog = os.path.join(OUT, f'{scenario}-paste.log')
open(pastelog, 'w').close()
events = []
inj = None
def cmd(c, label=None):
    inj.stdin.write(c + '\n'); inj.stdin.flush()
    # Every command answers within its own duration; a silent injector would otherwise hang the run.
    if not select.select([inj.stdout], [], [], 10)[0]: raise RuntimeError(f'injector did not answer {c!r}')
    t = int(inj.stdout.readline().split()[1])
    if label: events.append({'t': t, 'event': label})
    return t
def sleep(ms): time.sleep(ms / 1000)
def service_state():
    active = subprocess.run(['systemctl', '--user', 'is-active', 'quick-region-overlay.service'], capture_output=True, text=True, timeout=20).stdout.strip()
    pid = subprocess.run(['systemctl', '--user', 'show', '-P', 'MainPID', 'quick-region-overlay.service'], capture_output=True, text=True, timeout=20).stdout.strip()
    owners = subprocess.run(['qdbus6', '--literal', 'org.kde.kglobalaccel', '/kglobalaccel', 'org.kde.KGlobalAccel.getGlobalShortcutsByKey', '301989971'], capture_output=True, text=True, timeout=20).stdout
    owner = 'quick-region-overlay' if '"quick-region-overlay"' in owners else 'other/none'
    return f'{active} pid={pid} key={owner}'
def shortcut(label='press'):
    cmd('key KEY_LEFTMETA 1'); cmd('key KEY_LEFTSHIFT 1'); cmd('key KEY_S 1', label); cmd('key KEY_S 0'); cmd('key KEY_LEFTSHIFT 0'); cmd('key KEY_LEFTMETA 0')
def tap(key, label=None): cmd(f'key {key} 1', label); cmd(f'key {key} 0')
def dp4_position():
    outputs = json.loads(subprocess.run(['kscreen-doctor', '-j'], capture_output=True, text=True, timeout=20, check=True).stdout)['outputs']
    pos = next(o['pos'] for o in outputs if o['name'] == 'DP-4')
    return f"{pos['x']},{pos['y']}"
def move_dp4(position): subprocess.run(['kscreen-doctor', f'output.DP-4.position.{position}'], capture_output=True, timeout=20, check=True)
def drag(x0, y0, x1, y1, label):
    shortcut(); sleep(600)
    cmd(f'abs {x0} {y0}'); sleep(50); cmd('btn 1', label); cmd(f'drag {x0} {y0} {x1} {y1} 40 200'); cmd('btn 0', 'release'); sleep(800)
def kwin_script(*args):
    return subprocess.run(['qdbus6', 'org.kde.KWin', *args], capture_output=True, text=True, timeout=20)
def stop_process(process, quit_input=None):
    # Ask nicely (the injector closes its uinput devices on quit or end of input), then terminate, then kill.
    if process.poll() is None and quit_input is not None:
        try: process.stdin.write(quit_input); process.stdin.flush()
        except (BrokenPipeError, ValueError): pass
    if process.stdin:
        try: process.stdin.close()
        except BrokenPipeError: pass
    for stop in (None, process.terminate, process.kill):
        # sudo may refuse signals from the user; the injector also exits at the end of its input.
        try:
            if stop: stop()
        except PermissionError: pass
        try: process.wait(timeout=10); return
        except subprocess.TimeoutExpired: pass
    raise RuntimeError(f'process {process.pid} did not exit')
def park_pointer():
    try: cmd('abs 3200 1000')
    except Exception as error: print(f'could not park the pointer: {error}', file=sys.stderr)
# Each resource registers its cleanup as soon as it exists; ExitStack runs every callback even if one fails.
with contextlib.ExitStack() as cleanup:
    dp4_home = dp4_position() if scenario == 'layout' else None
    if dp4_home:
        print(f'If this run is interrupted, put DP-4 back with: kscreen-doctor output.DP-4.position.{dp4_home}', flush=True)
        cleanup.callback(lambda: dp4_position() != dp4_home and move_dp4(dp4_home))
    kwin_script('/Scripting', 'org.kde.kwin.Scripting.unloadScript', 'qromap')
    sid = kwin_script('/Scripting', 'org.kde.kwin.Scripting.loadScript', os.path.join(H, 'mapwatch.js'), 'qromap').stdout.strip()
    cleanup.callback(kwin_script, '/Scripting', 'org.kde.kwin.Scripting.unloadScript', 'qromap')
    subprocess.run(['qdbus6', 'org.kde.KWin', f'/Scripting/Script{sid}', 'org.kde.kwin.Script.run'], check=True, timeout=20)
    pastefile = cleanup.enter_context(open(pastelog, 'w'))
    watch = subprocess.Popen([os.path.expanduser('~/.cache/agent-scratch/desktop-config/probe-build/probe')], stdout=pastefile, env=dict(os.environ, QT_QPA_PLATFORM='wayland'))
    cleanup.callback(lambda: (sleep(500), stop_process(watch)))
    inj = subprocess.Popen(['sudo', '-n', 'python3', '-I', os.path.join(H, 'injector.py'), '7920', '1440'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    cleanup.callback(stop_process, inj, 'quit\n')
    cleanup.callback(park_pointer)
    if not select.select([inj.stdout], [], [], 20)[0] or inj.stdout.readline().strip() != 'ready':
        raise RuntimeError('injector did not start (sudo -n or uinput failed)')
    sleep(1500)  # let the wl-paste watcher report the existing clipboard first
    for trial in range(1, trials + 1):
        events.append({'t': now(), 'event': 'trial', 'trial': trial})
        if scenario == 'measure':
            x0, y0 = 3000 + 20 * trial, 500
            cmd(f'abs {x0} {y0}'); sleep(400)
            shortcut(); sleep(700)
            cmd(f'abs {x0+1} {y0+1}'); cmd('key KEY_LEFTSHIFT 1', 'shift-down'); sleep(100)
            cmd('btn 1', 'button-down')
            cmd(f'drag {x0+1} {y0+1} {x0+400+trial*50} {y0+250} 100 500')
            cmd('btn 0', 'release'); cmd('key KEY_LEFTSHIFT 0'); sleep(1500)
        elif scenario == 'behaviour':
            cmd('abs 2400 300'); sleep(300)
            shortcut(); sleep(600)
            cmd('btn 1'); cmd('btn 0', 'click-only'); sleep(300)
            shortcut('second-press'); sleep(600)
            tap('KEY_ESC', 'escape'); sleep(600)
            shortcut(); sleep(600)
            cmd('btn 1', 'cross-monitor-down'); cmd('drag 2400 300 2900 600 60 300'); cmd('btn 0', 'release'); sleep(800)
        elif scenario == 'magnifier':
            cmd('abs 3600 700'); sleep(300)
            shortcut(); sleep(600)
            cmd('abs 3610 705'); cmd('key KEY_LEFTSHIFT 1', 'shift-down'); cmd('abs 3612 706'); sleep(300)
            subprocess.run(['spectacle', '-b', '-n', '-f', '-o', os.path.join(OUT, 'magnifier.png')], timeout=20)
            events.append({'t': now(), 'event': 'screenshot-taken'})
            cmd('key KEY_LEFTSHIFT 0'); sleep(200)
            cmd('btn 1'); cmd('drag 3612 706 3200 500 40 200'); cmd('key KEY_LEFTSHIFT 1'); cmd('drag 3200 500 3150 480 10 50'); sleep(300)
            subprocess.run(['spectacle', '-b', '-n', '-f', '-o', os.path.join(OUT, 'magnifier-drag.png')], timeout=20)
            cmd('key KEY_LEFTSHIFT 0'); tap('KEY_ESC', 'escape'); cmd('btn 0'); sleep(500)
        elif scenario == 'latency':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(1200)
            tap('KEY_ESC', 'escape'); sleep(1200)
        elif scenario == 'gap':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(700)
            tap('KEY_ESC', 'escape'); sleep(600)
        elif scenario == 'busygap':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(700)
            shortcut('second-press'); sleep(600)
            tap('KEY_ESC', 'escape'); sleep(600)
            shortcut(); sleep(1500)
            tap('KEY_ESC', 'escape2'); sleep(1500)
        elif scenario == 'presslag':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(700)
            cmd('btn 1', 'abs-down'); sleep(200); cmd('btn 0', 'abs-up'); sleep(300)
            cmd('rbtn 1', 'rel-down'); sleep(200); cmd('rbtn 0', 'rel-up'); sleep(300)
            tap('KEY_ESC', 'escape'); sleep(500)
        elif scenario == 'magtext':
            cmd('abs 2980 357'); sleep(300)
            subprocess.run(['spectacle', '-b', '-n', '-f', '-o', os.path.join(OUT, 'reference.png')], timeout=20)
            shortcut(); sleep(600)
            cmd('abs 2981 357'); cmd('key KEY_LEFTSHIFT 1', 'shift-down'); cmd('abs 2980 357'); sleep(300)
            subprocess.run(['spectacle', '-b', '-n', '-f', '-o', os.path.join(OUT, 'magtext.png')], timeout=20)
            cmd('key KEY_LEFTSHIFT 0'); tap('KEY_ESC', 'escape'); sleep(500)
        elif scenario == 'layout':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(700)
            events.append({'t': now(), 'event': 'move-dp4-start'})
            x, y = map(int, dp4_home.split(','))
            move_dp4(f'{x},{y - 195 if y >= 195 else y + 195}')
            events.append({'t': now(), 'event': 'move-dp4-done'}); sleep(1500)
            move_dp4(dp4_home)
            events.append({'t': now(), 'event': 'restore-dp4-done'}); sleep(1500)
            shortcut('press-after-layout'); sleep(700)
            cmd('btn 1'); cmd('drag 5900 300 6100 450 40 200'); cmd('btn 0', 'release'); sleep(800)
        elif scenario == 'directions':
            # The same 401x251 span dragged in all four directions, then drags inverted by 1 px on one axis.
            drag(3000, 500, 3400, 750, 'drag-down-right'); drag(3400, 750, 3000, 500, 'drag-up-left')
            drag(3000, 750, 3400, 500, 'drag-up-right'); drag(3400, 500, 3000, 750, 'drag-down-left')
            drag(3000, 500, 3400, 499, 'drag-1px-up'); drag(3400, 500, 3399, 900, 'drag-1px-left')
        elif scenario == 'shiftheld':
            # Shift held from the shortcut, pointer still: the magnifier should show without any motion.
            cmd('abs 3600 700'); sleep(300)
            cmd('key KEY_LEFTMETA 1'); cmd('key KEY_LEFTSHIFT 1'); cmd('key KEY_S 1', 'press'); cmd('key KEY_S 0'); cmd('key KEY_LEFTMETA 0'); sleep(700)
            subprocess.run(['spectacle', '-b', '-n', '-f', '-o', os.path.join(OUT, 'shiftheld.png')], timeout=20)
            events.append({'t': now(), 'event': 'screenshot-taken'})
            cmd('key KEY_LEFTSHIFT 0'); tap('KEY_ESC', 'escape'); sleep(500)
        elif scenario == 'secondlaunch':
            # Only the service runs the daemon. A no-argument launch prints usage and registers nothing; a manual --daemon
            # exits 3 while the service runs; --capture asks the service over D-Bus, which starts it if it was stopped.
            # Run once with the service running and once after systemctl --user stop quick-region-overlay.service.
            daemon = os.path.expanduser('~/.local/libexec/quick-region-overlay'); env = dict(os.environ, QT_QPA_PLATFORM='wayland')
            events.append({'t': now(), 'event': 'service-before', 'message': service_state()})
            r = subprocess.run([daemon], env=env, capture_output=True, text=True, timeout=20)
            events.append({'t': now(), 'event': 'no-arg-instance', 'message': f'exit={r.returncode} {r.stderr.strip().splitlines()[0] if r.stderr.strip() else ""}'})
            if service_state().startswith('active'):
                r = subprocess.run([daemon, '--daemon'], env=env, capture_output=True, text=True, timeout=20)
                events.append({'t': now(), 'event': 'manual-daemon-instance', 'message': f'exit={r.returncode} {r.stderr.strip()}'})
                cmd('abs 3000 600'); sleep(300)
                shortcut('press'); sleep(700)
                tap('KEY_ESC', 'escape'); sleep(500)
            cmd('abs 3000 600'); sleep(300)
            r = subprocess.run([daemon, '--capture'], env=env, capture_output=True, text=True, timeout=30)
            events.append({'t': now(), 'event': 'capture-instance', 'message': f'exit={r.returncode} {r.stderr.strip()}'}); sleep(700)
            tap('KEY_ESC', 'escape-forwarded'); sleep(500)
            events.append({'t': now(), 'event': 'service-after-capture', 'message': service_state()})
            shortcut('press'); sleep(700)
            tap('KEY_ESC', 'escape'); sleep(500)
        elif scenario == 'press':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(700)
            tap('KEY_ESC', 'escape'); sleep(500)
        elif scenario == 'spectacle':
            cmd('abs 3000 600'); sleep(300)
            shortcut(); sleep(1500)
            cmd('btn 1', 'button-down'); cmd('drag 3000 600 3333 777 40 300'); cmd('btn 0', 'release'); sleep(2000)
        elif scenario == 'print':
            cmd('abs 3000 600'); sleep(300)
            tap('KEY_SYSRQ', 'print'); sleep(1500)
            cmd('btn 1', 'button-down'); cmd('drag 3000 600 3300 800 40 300'); cmd('btn 0', 'release'); sleep(2500)
        elif scenario == 'printclick':
            # A click without a drag in Print's overlay logs click-no-capture and leaves the overlay open, the second
            # click too; Escape closes it.
            cmd('abs 3000 600'); sleep(300)
            tap('KEY_SYSRQ', 'print'); sleep(1500)
            cmd('btn 1'); cmd('btn 0', 'click-only'); sleep(3000)
            cmd('btn 1'); cmd('btn 0', 'click-only-again'); sleep(3000)
            tap('KEY_ESC', 'escape'); sleep(1000)
journal = subprocess.run(['journalctl', '--user', '--since', start_wall, '--no-pager', '-o', 'json'], capture_output=True, text=True, timeout=60).stdout
for line in journal.splitlines():
    j = json.loads(line); m = j.get('MESSAGE')
    if not isinstance(m, str): continue
    t = int(j['__REALTIME_TIMESTAMP']) // 1000
    if 'QROMAP' in m: events.append({'t': int(m.split()[3]), 'event': 'kwin', 'message': m.split('QROMAP ')[1]})
    elif j.get('_SYSTEMD_USER_UNIT') in ('quick-region-overlay.service', 'quick-region-clipboard.service') and m[:13].isdigit(): events.append({'t': int(m[:13]), 'event': 'daemon', 'message': m[14:]})
    elif 'spectacle' in j.get('SYSLOG_IDENTIFIER', '') : events.append({'t': t, 'event': 'spectacle', 'message': m})
for line in open(pastelog):
    f = line.split()
    if len(f) == 3: events.append({'t': int(f[0]), 'event': 'probe-changed'}); events.append({'t': int(f[1]), 'event': 'probe-png-decoded', 'message': f[2]})
events.sort(key=lambda e: e['t'])
path = os.path.join(OUT, f'{scenario}-{time.strftime("%H%M%S")}.jsonl')
with open(path, 'w') as f:
    for e in events: f.write(json.dumps(e) + '\n')
t0 = events[0]['t']
for e in events: print(e['t'] - t0, e['event'], e.get('message', e.get('bytes', e.get('trial', ''))))
print(path)
