"""Read-only/offline DSP audit; no device capture/playback or product changes.

SWResample tests call the supplied application's actual DLLs. Guard/fade cases
are explicitly arithmetic models of the audited C++ (not product integration).
Requires numpy. Prints JSON evidence; redirect stdout to an ignored log path.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path

import numpy as np


class Layout(C.Structure):
    _fields_ = [("order", C.c_int), ("channels", C.c_int),
                ("mask", C.c_uint64), ("opaque", C.c_void_p)]


parser = argparse.ArgumentParser()
parser.add_argument("--dll-dir", type=Path, required=True)
args = parser.parse_args()
directory = args.dll_dir.resolve()
search = os.add_dll_directory(str(directory))
av = C.CDLL(str(directory / "avutil-61.dll"))
sw = C.CDLL(str(directory / "swresample-7.dll"))
ptr = C.c_void_p
sw.swr_alloc_set_opts2.argtypes = [C.POINTER(ptr), C.POINTER(Layout), C.c_int,
                                  C.c_int, C.POINTER(Layout), C.c_int,
                                  C.c_int, C.c_int, ptr]
sw.swr_alloc_set_opts2.restype = C.c_int
sw.swr_init.argtypes = [ptr]
sw.swr_init.restype = C.c_int
sw.swr_free.argtypes = [C.POINTER(ptr)]
sw.swr_get_delay.argtypes = [ptr, C.c_int64]
sw.swr_get_delay.restype = C.c_int64
sw.swr_set_compensation.argtypes = [ptr, C.c_int, C.c_int]
sw.swr_set_compensation.restype = C.c_int
sw.swr_convert.argtypes = [ptr, C.POINTER(ptr), C.c_int,
                           C.POINTER(ptr), C.c_int]
sw.swr_convert.restype = C.c_int
av.av_version_info.restype = C.c_char_p


def checked(code):
    if code < 0:
        raise RuntimeError(f"FFmpeg error {code}")


class Resampler:
    def __init__(self):
        self.ctx = ptr()
        layout = Layout(1, 2, 3, None)
        # Packed S16 -> packed float, stereo 48 kHz: actual capture format.
        checked(sw.swr_alloc_set_opts2(C.byref(self.ctx), C.byref(layout), 3,
                                      48000, C.byref(layout), 1, 48000, 0, None))
        checked(sw.swr_init(self.ctx))

    def convert(self, block):
        out = np.empty((2048, 2), dtype=np.float32)
        output = (ptr * 1)(out.ctypes.data)
        source = None if block is None else (ptr * 1)(block.ctypes.data)
        count = sw.swr_convert(self.ctx, output, len(out), source,
                               0 if block is None else len(block))
        checked(count)
        return out[:count].copy()

    def compensation(self, delta):
        checked(sw.swr_set_compensation(self.ctx, delta, 48000 if delta else 0))

    def delay(self):
        return sw.swr_get_delay(self.ctx, 48000)

    def close(self):
        sw.swr_free(C.byref(self.ctx))


def signal(frequency, blocks=200):
    t = np.arange(blocks * 480) / 48000
    mono = np.round(0.5 * 32768 * np.sin(2 * np.pi * frequency * t + 0.71)).astype(np.int16)
    return np.repeat(mono[:, None], 2, axis=1)


def run(data, reset_at=None, zero_at=None, compensated=True):
    s = Resampler()
    chunks = []
    events = []
    for i, block in enumerate(data.reshape(-1, 480, 2)):
        if compensated and i % 25 == 0 and (zero_at is None or i < zero_at):
            s.compensation(-60)  # -1250 ppm; near latest user's log.
        if i == zero_at:
            events.append({"block": i, "pending_input_frames": s.delay(),
                           "output_frame": sum(len(c) for c in chunks)})
            if reset_at == i:
                s.close()
                s = Resampler()
            else:
                s.compensation(0)
        chunks.append(s.convert(block))
    pending = s.delay()
    chunks.append(s.convert(None))
    s.close()
    return np.concatenate(chunks), events, pending


def sine_residual(samples, frequency, refine=False):
    requested_frequency = frequency
    if refine:
        # Compensation increments are integers in FFmpeg. Fitting the nominal
        # frequency alone mistakes fractional-rate rounding for broadband noise.
        small = samples[::4]
        times = np.arange(len(small)) * 4 / 48000
        def cost(f):
            basis = np.stack([np.sin(2*np.pi*f*times),
                              np.cos(2*np.pi*f*times), np.ones(len(times))], axis=1)
            weights = np.linalg.lstsq(basis, small, rcond=None)[0]
            return float(np.mean((small-basis@weights)**2))
        lo, hi = frequency-.1, frequency+.1
        for _ in range(38):
            left, right = lo+(hi-lo)/3, hi-(hi-lo)/3
            if cost(left) < cost(right):
                hi = right
            else:
                lo = left
        frequency = (lo+hi)/2
    t = np.arange(len(samples)) / 48000
    basis = np.stack([np.sin(2*np.pi*frequency*t),
                      np.cos(2*np.pi*frequency*t), np.ones(len(t))], axis=1)
    weights = np.linalg.lstsq(basis, samples, rcond=None)[0]
    error = samples - basis @ weights
    rms = float(np.sqrt(np.mean(error**2)))
    return {"nominal_frequency_hz": requested_frequency, "fitted_frequency_hz": frequency,
            "fitted_amplitude": float(np.hypot(*weights[:2])),
            "residual_dbfs": float(20*np.log10(max(rms, 1e-20)))}


def model_guard(block_peaks):
    gain = 1.0
    rows = []
    for peak in block_peaks:
        target = .995/peak if peak > .995 else 1.0
        gain = target if target < gain else min(1.0, gain+.05)
        applied = gain if target < .99999 else 1.0
        raw = peak * applied
        rows.append({"input_peak": peak, "state_gain": gain, "applied_gain": applied,
                     "pre_clamp_peak": raw, "hard_clip": raw > 1.0})
    return rows


results = {"kind": "offline audit, NOT acoustic or full-player acceptance",
           "ffmpeg_version": av.av_version_info().decode(),
           "dlls": {name: hashlib.file_digest(open(directory/name, "rb"), "sha256").hexdigest()
                    for name in ["avutil-61.dll", "swresample-7.dll"]}}
stable = []
for f in [1000, 4000, 8000, 16000]:
    data = signal(f)
    identity, _, _ = run(data, compensated=False)
    compensated, _, _ = run(data)
    stable.append({"input_hz": f,
                   "identity_max_abs_error": float(np.max(np.abs(identity-data/32768))),
                   # Omit startup/end transients; expected step = 1.00125.
                   "fixed_compensation": sine_residual(compensated[2000:-2000, 0], f*1.00125, refine=True)})
results["stable_actual_swr"] = stable
data = signal(4000)
kept, events, _ = run(data, zero_at=100)
replaced, _, _ = run(data, zero_at=100, reset_at=100)
edge = events[0]["output_frame"]
results["zero_crossing_actual_swr"] = {
    "event": events[0], "kept_frames_with_drain": len(kept),
    "replaced_frames_with_drain": len(replaced),
    "lost_output_frames": len(kept)-len(replaced),
    "first_post_boundary_reference": float(kept[edge, 0]),
    "first_post_boundary_replaced": float(replaced[edge, 0]),
    "max_difference_first_48_frames": float(np.max(np.abs(kept[edge:edge+48]-replaced[edge:edge+48])))}
results["guard_arithmetic_model_constant_peak"] = model_guard([1.05]*4)
results["guard_arithmetic_model_release"] = model_guard([1.1, .9, .9])
results["fade_arithmetic_model_false_empty_pull"] = {
    "queued_endpoint_frames_at_empty_pull": 480,
    "next_callback_before_queue_drain": True,
    "actual_playback_gap_frames": 0,
    "input_next_sample": .5, "new_fade_first_sample": .5/240,
    "gain_drop_db": float(20*np.log10(1/240)),
    "note": "Model of C++ trigger/envelope, not device loopback."}
assert all(row["identity_max_abs_error"] == 0 for row in stable)
assert results["zero_crossing_actual_swr"]["lost_output_frames"] > 0
assert results["guard_arithmetic_model_constant_peak"][1]["hard_clip"]
print(json.dumps(results, indent=2))
