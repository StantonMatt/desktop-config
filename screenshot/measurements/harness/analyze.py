#!/usr/bin/python3 -I
# Per-trial latencies from run.py output. Usage: analyze.py <events.jsonl>...
# key->daemon: injected S press to the daemon's shortcut-pressed line (KGlobalAccel signal received).
# first/all overlays: S press to KWin's windowAdded for the first and last overlay surface.
# capture: daemon capture-request to the last overlay-shown line. readable: button release to a native client's decoded PNG.
import json, statistics, sys
rows = []
for path in sys.argv[1:]:
    events = [json.loads(l) for l in open(path)]
    presses = [i for i, e in enumerate(events) if e['event'] in ('press', 'press-after-layout')]
    for n, i in enumerate(presses):
        end = presses[n + 1] if n + 1 < len(presses) else len(events)
        window = events[i:end]; t0 = events[i]['t']
        first = lambda pred: next((e['t'] for e in window if pred(e)), None)
        msg = lambda e: e.get('message', '')
        shortcut = first(lambda e: e['event'] == 'daemon' and msg(e).startswith('shortcut-pressed'))
        request = first(lambda e: e['event'] == 'daemon' and msg(e).startswith('capture-request'))
        added = [e['t'] for e in window if e['event'] == 'kwin' and msg(e).startswith('added') and 'quick-region-overlay' in msg(e)]
        shown = [e['t'] for e in window if e['event'] == 'daemon' and msg(e).startswith('overlay-shown')]
        release = first(lambda e: e['event'] == 'release')
        decoded = next((e['t'] for e in window if e['event'] == 'probe-png-decoded' and release and e['t'] >= release), None)
        rows.append({'file': path.rsplit('/', 1)[-1], 'trial': n + 1,
                     'key_to_daemon_ms': shortcut - t0 if shortcut else None,
                     'capture_request_to_all_shown_ms': shown[-1] - request if shown and request else None,
                     'press_first_overlay_ms': min(added) - t0 if added else None,
                     'press_all_overlays_ms': max(added) - t0 if len(added) >= 3 else None,
                     'release_native_png_decoded_ms': decoded - release if decoded and release else None})
for r in rows: print(json.dumps(r))
for key in ('key_to_daemon_ms', 'capture_request_to_all_shown_ms', 'press_first_overlay_ms', 'press_all_overlays_ms', 'release_native_png_decoded_ms'):
    v = [r[key] for r in rows if r[key] is not None]
    if v: print(f'{key}: n={len(v)} median={statistics.median(v)} min={min(v)} max={max(v)}')
