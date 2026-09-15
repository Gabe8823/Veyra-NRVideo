"""Inspect explicitly recorded capture PCM and per-process Windows loopback.

Requires numpy. This diagnoses exact-zero discontinuities and endpoint padding;
it is not a perceptual score or proof about an acoustic output device.
"""
import argparse
import json
from pathlib import Path
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("prefix", type=Path)
parser.add_argument("--start-seconds", type=float, default=5)
parser.add_argument("--end-seconds", type=float, default=20)
args = parser.parse_args()
prefix = str(args.prefix)
meta = json.loads(Path(prefix + ".json").read_text())
writes = np.atleast_1d(np.genfromtxt(prefix + ".writes.csv", delimiter=",", names=True))
if not len(writes) or not np.isfinite(writes["channels"]).all():
    raise ValueError("No endpoint writes: this recording cannot verify playback")
channels = int(writes["channels"][0])
if channels != meta["channels"] or channels != 2:
    raise ValueError("This comparison requires unchanged stereo mapping")
pulled = np.fromfile(prefix + ".pulled.f32", dtype="<f4").reshape(-1, channels)
rendered = np.fromfile(prefix + ".rendered.f32", dtype="<f4").reshape(-1, channels)
loopback = np.fromfile(prefix + ".loopback.f32", dtype="<f4").reshape(-1, 2)
start = int(args.start_seconds * 48000)
end = int(args.end_seconds * 48000)
if not 0 <= start < end <= len(loopback):
    raise ValueError("Requested loopback interval is unavailable")
zero = np.all(loopback == 0, axis=1)
# Reject naturally quiet quantized samples: only count a sudden exact stereo
# zero immediately after a sample whose peak amplitude exceeds 0.002.
edge = zero[1:-1] & ~zero[:-2] & (np.max(abs(loopback[:-2]), axis=1) > .002)
positions = np.flatnonzero(edge) + 1
positions = positions[(positions >= start) & (positions < end)]
residues, counts = np.unique(positions % 480, return_counts=True)
steady = writes[writes["frame"] > 48000]
print(json.dumps({
    "prefix": prefix,
    "loopback_interval_seconds": [args.start_seconds, args.end_seconds],
    "hard_zero_edges": len(positions),
    "stereo_zero_frames": int(zero[start:end].sum()),
    "edge_residues_in_10ms": dict(zip(map(str, residues), map(int, counts))),
    "steady_writes": len(steady),
    "steady_empty_padding_writes": int((steady["padding"] == 0).sum()),
    "steady_padding_min_median_max_frames": np.quantile(steady["padding"], [0, .5, 1]).tolist(),
    "pull_render_equal_after_240_frame_gain_ramp": bool(
        len(pulled) == len(rendered) and len(rendered) > 240 and
        np.array_equal(pulled[240:], rendered[240:])),
    "limitations": "Different music segments; no perceptual/acoustic quality claim. Empty padding alone does not prove an audible gap."
}, ensure_ascii=False, indent=2))
