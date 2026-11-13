#!/usr/bin/env python3
"""Packs sounds/<Type>/NN.wav (mono, 16 bit, 44.1 kHz) into sounds.pak: 'ALSP', u32 count, then per sample char[12] type, u32 frames, int16 data."""
import glob, os, struct, wave, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
out = bytearray()
files = sorted(glob.glob(os.path.join(root, 'sounds', '*', '*.wav')))
out += b'ALSP' + struct.pack('<I', len(files))
for p in files:
    t = os.path.basename(os.path.dirname(p)).encode()[:11]
    w = wave.open(p); assert w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == 44100
    d = w.readframes(w.getnframes())
    out += t.ljust(12, b'\0') + struct.pack('<I', w.getnframes()) + d
open(os.path.join(root, 'sounds.pak'), 'wb').write(out)
print(len(files), 'samples', len(out), 'bytes')
