# Root-side uinput injector. Reads commands on stdin; prints "ok <epoch_ms>" after each.
import sys, time
from evdev import UInput, ecodes as e, AbsInfo
W, H = int(sys.argv[1]), int(sys.argv[2])
kbd = UInput({e.EV_KEY: [e.KEY_LEFTMETA, e.KEY_LEFTSHIFT, e.KEY_S, e.KEY_ESC, e.KEY_SYSRQ]}, name='qro-test-keyboard')
ptr = UInput({e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
              e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, W - 1, 0, 0, 0)), (e.ABS_Y, AbsInfo(0, 0, H - 1, 0, 0, 0))]},
             name='qro-test-pointer', input_props=[e.INPUT_PROP_POINTER] if hasattr(e, 'INPUT_PROP_POINTER') else None)
rel = UInput({e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE], e.EV_REL: [e.REL_X, e.REL_Y]}, name='qro-test-mouse')
time.sleep(1.0)
print('ready', flush=True)
for line in sys.stdin:
    parts = line.split()
    if not parts: continue
    cmd = parts[0]
    if cmd == 'key':
        kbd.write(e.EV_KEY, getattr(e, parts[1]), int(parts[2])); kbd.syn()
    elif cmd == 'abs':
        ptr.write(e.EV_ABS, e.ABS_X, int(parts[1])); ptr.write(e.EV_ABS, e.ABS_Y, int(parts[2])); ptr.syn()
    elif cmd == 'btn':
        ptr.write(e.EV_KEY, e.BTN_LEFT, int(parts[1])); ptr.syn()
    elif cmd == 'rbtn':
        rel.write(e.EV_KEY, e.BTN_LEFT, int(parts[1])); rel.syn()
    elif cmd == 'drag':  # drag x0 y0 x1 y1 steps duration_ms
        x0, y0, x1, y1, n, ms = map(int, parts[1:])
        t0 = time.monotonic()
        for i in range(1, n + 1):
            ptr.write(e.EV_ABS, e.ABS_X, x0 + (x1 - x0) * i // n); ptr.write(e.EV_ABS, e.ABS_Y, y0 + (y1 - y0) * i // n); ptr.syn()
            target = t0 + ms / 1000 * i / n
            while time.monotonic() < target: time.sleep(0.0005)
    elif cmd == 'quit':
        break
    print('ok', int(time.time() * 1000), flush=True)
kbd.close(); ptr.close(); rel.close()
