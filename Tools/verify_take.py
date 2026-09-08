#!/usr/bin/env python3
"""Checks a finished take on disk the way a listener would: every stem and the
mix decode, hold the length the take ran, carry signal where a microphone was
carrying signal, and the mirror copy matches byte-for-byte in size.

    Tools/verify_take.py <session-folder> --seconds 10 [--bits 24]
                         [--tone NAME=HZ ...] [--mirror-root ~/RECORDINGS-MIRROR]

Exit status is non-zero on any failure, so a release script can gate on it.
"""
import argparse, glob, json, math, os, struct, sys

def parse_wav(path):
    d = open(path, 'rb').read()
    if len(d) < 12 or d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        return None, None, 'not a RIFF/WAVE file (%d bytes)' % len(d)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        tag = d[pos:pos + 4]
        size = struct.unpack('<I', d[pos + 4:pos + 8])[0]
        if tag == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[pos + 8:pos + 24])
        elif tag == b'data':
            data = (pos + 8, size)
            break
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        return None, None, 'missing fmt or data chunk'
    off, size = data
    if off + size > len(d):
        return None, None, 'data chunk claims %d bytes, file holds %d' % (size, len(d) - off)
    return fmt, d[off:off + size], None

def decode(raw, bits, channels, max_frames):
    bps = bits // 8
    n = min(len(raw) // (bps * channels), max_frames)
    out = []
    for i in range(n):
        p = i * bps * channels
        if bits == 16:
            out.append(struct.unpack('<h', raw[p:p + 2])[0] / 32768.0)
        elif bits == 24:
            v = raw[p] | (raw[p + 1] << 8) | (raw[p + 2] << 16)
            if v & 0x800000: v -= 1 << 24
            out.append(v / 8388608.0)
        else:
            out.append(struct.unpack('<i', raw[p:p + 4])[0] / 2147483648.0)
    return out

def goertzel(x, rate, hz):
    w = 2.0 * math.pi * hz / rate
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2

def tone_vote(samples, rate, candidates, block=64):
    """Which candidate tone each 64-sample block favours, by majority.

    Judged per block rather than over a second, because a virtual device with
    no clock floods the app and the recorded stream is contiguous only within
    a block. Inside a block the tone is whatever the microphone carried."""
    votes = {hz: 0 for hz in candidates}
    peak = 0.0
    for start in range(0, len(samples) - block, block):
        seg = samples[start:start + block]
        p = max(abs(s) for s in seg)
        peak = max(peak, p)
        if p < 0.05:
            continue
        best = max(candidates, key=lambda hz: goertzel(seg, rate, hz))
        votes[best] += 1
    return votes, peak

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--seconds', type=float, default=10.0, help='the take must hold at least this much audio')
    ap.add_argument('--bits', type=int, default=24)
    ap.add_argument('--tone', action='append', default=[], help='NAME=HZ: stems whose name contains NAME must carry that tone')
    ap.add_argument('--silent-ok', action='append', default=[],
                    help='NAME: stems whose name contains NAME are allowed to be silent '
                         '(a source with nothing plugged into it)')
    ap.add_argument('--mirror-root', default=os.path.expanduser('~/RECORDINGS-MIRROR'))
    ap.add_argument('--no-mirror', action='store_true')
    a = ap.parse_args()

    failures = []
    def check(ok, what):
        print(('  PASS  ' if ok else '  FAIL  ') + what)
        if not ok: failures.append(what)

    wavs = sorted(glob.glob(os.path.join(a.folder, '*.wav')))
    print('take:', a.folder)
    check(len(wavs) >= 2, '%d wav files (stems + MIX)' % len(wavs))
    check(os.path.basename(wavs[-1]) == 'MIX.wav' if wavs else False, 'MIX.wav present')

    tones = [(t.split('=')[0], float(t.split('=')[1])) for t in a.tone]
    sizes = {}
    for path in wavs:
        name = os.path.basename(path)
        fmt, raw, err = parse_wav(path)
        if err:
            check(False, '%s: %s' % (name, err)); continue
        ch, rate, bits = fmt[1], fmt[2], fmt[5]
        bps = bits // 8
        frames = len(raw) // (bps * ch)
        secs = frames / rate
        sizes[name] = len(raw)
        check(bits == a.bits, '%s: %d-bit as configured' % (name, bits))
        check(fmt[4] == ch * bps, '%s: block align %d matches %d ch x %d bytes' % (name, fmt[4], ch, bps))
        check(secs >= a.seconds, '%s: holds %.1f s (need >= %.1f s)' % (name, secs, a.seconds))
        samples = decode(raw, bits, ch, rate * 8)
        candidates = sorted(set(hz for _, hz in tones)) or [440.0]
        votes, peak = tone_vote(samples, rate, candidates)
        for key, want in tones:
            if key in name:
                check(peak > 0.15, '%s: carries signal (peak %.2f)' % (name, peak))
                total = sum(votes.values())
                share = votes.get(want, 0) / total if total else 0.0
                # 64 samples is 0.6 of a 440 Hz cycle, so Goertzel cannot tell
                # 440 from 1000 on every single block even on a perfect
                # recording -- a correct stem measures 75-82% here. The check
                # is that the stem carries ITS OWN source's tone, so it wants a
                # clear majority, not near-unanimity.
                check(share > 0.6, '%s: %.0f Hz on %.0f%% of blocks (want > 60%%)' % (name, want, share * 100))
        if (name != 'MIX.wav'
                and not any(key in name for key, _ in tones)
                and not any(key in name for key in a.silent_ok)):
            # A stem nobody named a tone for still has to hold audio: silent
            # stems are exactly the bug this gate missed before.
            check(peak > 0.15, '%s: carries signal (peak %.2f)' % (name, peak))
        if name == 'MIX.wav':
            check(peak > 0.1, 'MIX.wav: carries signal (peak %.2f)' % peak)

    meta = os.path.join(a.folder, 'session.json')
    mirror_ran = False
    if os.path.exists(meta):
        j = json.load(open(meta))
        check(bool(j.get('stopTimestamp')), 'session.json has a stop timestamp')
        mirror_ran = bool(j.get('mirrorActive'))
        if j.get('mirrorEnabled') and not mirror_ran:
            print('  NOTE  mirror was enabled but did not run (low space on the internal drive?)')
    else:
        check(False, 'session.json present')

    if not a.no_mirror and mirror_ran:
        mirror = os.path.join(a.mirror_root, os.path.basename(a.folder))
        check(os.path.isdir(mirror), 'mirror copy exists at %s' % mirror)
        if os.path.isdir(mirror):
            for name, size in sizes.items():
                m = os.path.join(mirror, name)
                fmt, raw, err = parse_wav(m) if os.path.exists(m) else (None, None, 'missing')
                check(err is None and len(raw) == size, 'mirror %s: same data size (%s)' % (name, 'ok' if err is None and len(raw) == size else err or len(raw)))

    print('\n%s (%d failing)' % ('ALL CHECKS PASSED' if not failures else 'FAILED', len(failures)))
    sys.exit(1 if failures else 0)

if __name__ == '__main__':
    main()
