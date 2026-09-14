#!/usr/bin/env python3
"""Check a take recorded while one microphone was unavailable.

    Tools/verify_partial_take.py <take-folder> <working-name> <dead-name>

A microphone that will not open no longer stops the rest of the rig recording.
That is only an improvement if the take it produces is HONEST about it, so this
asks four things:

  * the working microphone actually recorded
  * the dead one wrote silence, not stale audio or a held sample (SS 6.5)
  * every stem is the same length -- the take layout must not shift because a
    device is missing
  * the take's own record says WHY that track is empty (SS 0.1: silence nobody
    warned about is the whole failure this app is built against)
"""
import json
import os
import sys
import wave


def peak_of(path):
    with wave.open(path) as w:
        frames, width = w.getnframes(), w.getsampwidth()
        raw = w.readframes(frames)
    best = 0.0
    # Every 29th sample: enough to find a tone's peak, cheap enough for a gate.
    for i in range(0, max(0, len(raw) - width), width * 29):
        v = int.from_bytes(raw[i:i + width], 'little', signed=True) / float(1 << (8 * width - 1))
        best = max(best, abs(v))
    return best


def main():
    folder, working, dead = sys.argv[1], sys.argv[2], sys.argv[3]
    failures = []

    wavs = {f: os.path.join(folder, f)
            for f in os.listdir(folder) if f.lower().endswith('.wav')}

    working_path = next((p for f, p in wavs.items() if working in f), None)
    dead_path = next((p for f, p in wavs.items() if dead in f), None)

    if working_path is None:
        failures.append('the working microphone (%s) produced no file at all' % working)
    else:
        peak = peak_of(working_path)
        if peak < 0.15:
            failures.append('the working microphone recorded silence (peak %.2f)' % peak)
        else:
            print('  PASS  the working microphone recorded (peak %.2f)' % peak)

    if dead_path is None:
        # No stem at all is a different design than a silent one, and not the
        # one SS 6.5 describes; say so rather than quietly accepting it.
        failures.append('the unavailable microphone (%s) has no stem; the take layout dropped it'
                        % dead)
    else:
        peak = peak_of(dead_path)
        if peak > 0.02:
            failures.append('the unavailable microphone wrote audio (peak %.2f), not silence' % peak)
        else:
            print('  PASS  the unavailable microphone wrote silence, not stale audio')

    lengths = {}
    for f, p in wavs.items():
        with wave.open(p) as w:
            lengths[f] = w.getnframes()
    if len(set(lengths.values())) > 1:
        failures.append('stems are not frame-locked: %s' % lengths)
    elif lengths:
        print('  PASS  every stem is frame-locked (%d frames)' % next(iter(lengths.values())))

    meta = json.load(open(os.path.join(folder, 'session.json')))
    named = [d for d in (meta.get('dropouts') or [])
             if 'could not be opened' in d.get('description', '')]
    if not named:
        failures.append("the take's record never says why the unavailable "
                        "microphone's track is silent")
    else:
        print('  PASS  the take records why that track is silent (%s)'
              % (named[0].get('deviceUsbId') or '?'))

    for f in failures:
        print('  FAIL  ' + f)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
