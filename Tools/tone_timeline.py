#!/usr/bin/env python3
"""Where in a stem the tone is and is not.

verify_take.py votes each 64-sample block between the fixture's tones and
wants a clear majority for the stem's own. When that fails on a machine you
cannot sit at, the share alone says nothing about the shape of the failure:
a click in every block and a wrong tone for whole seconds look the same.
This prints the vote per second, and a map of the first seconds a block at a
time, so the pattern can be read off a CI log.

  Tools/tone_timeline.py <stem.wav> <want-hz> <other-hz> [map-seconds]
  Tools/tone_timeline.py --seams <stem.wav> <want-hz>

Map key: '1' the stem's own tone, '.' the other, '_' quiet (peak < 0.05).

--seams counts the places where the tone stops being one sine: a sample
that a pure sine of that frequency could not have produced from the two
before it. Silence written for a dry ring is not a seam -- the tone stops
and starts again, and the app reports that loss itself -- so a break next
to zero samples is not counted. Anything else is audio out of order: a
sample repeated, skipped or replaced, which no counter in the app can see
and which a per-block vote only notices once there is one in most blocks.
"""
import math
import struct
import sys


def read_wav(path):
    raw = open(path, 'rb').read()
    i, fmt, data = 12, None, b''
    while i + 8 <= len(raw):
        cid = raw[i:i + 4]
        size = struct.unpack('<I', raw[i + 4:i + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', raw[i + 8:i + 24])
        elif cid == b'data':
            data = raw[i + 8:i + 8 + size]
            break
        i += 8 + size + (size & 1)
    channels, rate, bits = fmt[1], fmt[2], fmt[5]
    width = bits // 8
    frames = len(data) // (width * channels)
    if width == 3:
        samples = [int.from_bytes(data[k * width * channels:k * width * channels + 3], 'little', signed=True) / 8388608.0
                   for k in range(frames)]
    elif width == 2:
        samples = [struct.unpack_from('<h', data, k * width * channels)[0] / 32768.0 for k in range(frames)]
    else:
        samples = [struct.unpack_from('<i', data, k * width * channels)[0] / 2147483648.0 for k in range(frames)]
    return samples, rate


def goertzel(seg, rate, hz):
    c = 2.0 * math.cos(2.0 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for v in seg:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2


def count_seams(samples, rate, hz):
    c = 2.0 * math.cos(2.0 * math.pi * hz / rate)
    seams = []
    for n in range(2, len(samples)):
        if abs(samples[n] - (c * samples[n - 1] - samples[n - 2])) <= 0.02:
            continue
        if any(samples[k] == 0.0 for k in range(max(0, n - 4), min(len(samples), n + 4))):
            continue
        if seams and n - seams[-1] <= 4:
            continue
        seams.append(n)
    return seams


def main():
    if sys.argv[1] == '--seams':
        path, want = sys.argv[2], float(sys.argv[3])
        samples, rate = read_wav(path)
        seams = count_seams(samples, rate, want)
        print("%s: %d seams in %.1f s%s" % (path.split('/')[-1], len(seams), len(samples) / rate,
                                             ("; first at " + ", ".join("%.2f s" % (n / rate) for n in seams[:8])) if seams else ""))
        return

    path, want, other = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    map_seconds = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    samples, rate = read_wav(path)
    block = 64
    marks = []
    for start in range(0, len(samples) - block, block):
        seg = samples[start:start + block]
        if max(abs(v) for v in seg) < 0.05:
            marks.append('_')
        else:
            marks.append('1' if goertzel(seg, rate, want) >= goertzel(seg, rate, other) else '.')

    per_second = rate // block
    print("%s: %d blocks, %.1f s" % (path.split('/')[-1], len(marks), len(marks) * block / rate))
    for sec in range(0, len(marks) // per_second):
        chunk = marks[sec * per_second:(sec + 1) * per_second]
        own, oth, quiet = chunk.count('1'), chunk.count('.'), chunk.count('_')
        print("  s%03d own %3d other %3d quiet %3d" % (sec, own, oth, quiet))
    print("  map, %d blocks a row (%.3f s):" % (per_second, per_second * block / rate))
    for sec in range(0, min(map_seconds, len(marks) // per_second)):
        print("  " + ''.join(marks[sec * per_second:(sec + 1) * per_second]))


if __name__ == '__main__':
    main()
