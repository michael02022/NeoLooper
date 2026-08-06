"""
Rotation Loop: a "unison"/supersaw-style thickener built from an *existing*
seamless loop, using phase rotation instead of pitch detune to tell the
layers apart.

Unlike freeze_loop.py and wavetable_extract.py, this method does no spectral
analysis of its own and never invents a loop -- it requires the input file
to already carry a defined loop region (an `smpl` RIFF chunk) that is itself
assumed to already be seamless (e.g. written by this project's own
write_smpl_loop, or by an external sampler/editor). Rotation Loop only
reshuffles that existing loop:

  1. Read the loop region straight out of the file: loop_buffer =
     x[loop_start:loop_end].
  2. Build 2-100 layers, each the SAME loop_buffer read starting from a
     different sample (a circular rotation, np.roll). This is exactly a
     wavetable oscillator's phase knob: identical waveform, identical
     spectrum, just a different entry point in the cycle -- so each layer
     sounds like the source but the layers are not in phase with each
     other. A `seed` controls which offsets get picked (seed=-1 -> fresh
     random offsets every run; any other int -> reproducible).
  3. Sum the layers. Because a rotation shifts every harmonic's phase by an
     amount proportional to that harmonic's frequency, random rotations
     make the layers behave like decorrelated sources for loudness purposes
     even though they're spectrally identical -- summing more of them
     thickens the timbre (comb-filtering between the many phase
     relationships) without a 1/n-per-layer loudness drop-off, PROVIDED the
     per-layer gain is chosen to compensate (see compute_layer_gains).
  4. Splice the combined multi-layer loop onto the file's own real attack
     (audio before loop_start) via a crossfade of adjustable length, using
     the same rotation-alignment trick as freeze_loop.build_seamless_loop:
     search for whichever circular rotation of the COMBINED loop best
     matches the real attack, and crossfade into that. Rotating the whole
     combined buffer by a further shift `s` is equivalent to rotating every
     individual layer by `s` (rotation and summation commute), so this
     splice-alignment step does not disturb the relative offsets between
     layers that step 2 set up.

Usage:
    python rotation_loop.py loop_out.wav                              # needs an smpl chunk already in the file
    python rotation_loop.py loop_out.wav --n-layers 16 --seed 7
    python rotation_loop.py loop_out.wav --layer-falloff-db 4.5        # manual per-layer attenuation instead of auto-leveling
    python rotation_loop.py in.wav --loop-start 44100 --loop-end 88200 # override when the file has no smpl chunk
"""
import argparse
import struct

import numpy as np
import soundfile as sf

from freeze_loop import _circular_best_shift, _crossfade_gains, _resolve_xfade_n, write_smpl_loop


def read_smpl_loop(path):
    """
    Parse the WAV file's own bytes for an `smpl` RIFF chunk and return
    (loop_start, loop_end, root_note), using this project's convention of an
    EXCLUSIVE loop_end (see write_smpl_loop). Returns None if the file has
    no smpl chunk, or the chunk declares zero sample loops.

    python-soundfile/libsndfile don't expose the smpl chunk through the
    Python API (SoundFile only hands back audio frames), so this walks the
    RIFF chunk list by hand -- the same byte layout write_smpl_loop already
    documents, just read back instead of written: dwEnd is stored INCLUSIVE
    in the file (RIFF spec), so loop_end here is dw_end + 1.
    """
    with open(path, "rb") as f:
        riff, _riff_size, wave = struct.unpack("<4sI4s", f.read(12))
        if riff != b"RIFF" or wave != b"WAVE":
            raise ValueError(f"{path}: not a RIFF/WAVE file")
        while True:
            header = f.read(8)
            if len(header) < 8:
                break
            chunk_id, chunk_size = struct.unpack("<4sI", header)
            body_start = f.tell()
            if chunk_id == b"smpl":
                body = f.read(chunk_size)
                root_note = struct.unpack_from("<I", body, 12)[0]
                num_loops = struct.unpack_from("<I", body, 28)[0]
                if num_loops < 1:
                    return None
                dw_start, dw_end = struct.unpack_from("<2I", body, 44)
                return dw_start, dw_end + 1, root_note
            f.seek(body_start + chunk_size + (chunk_size & 1))  # chunks are word-aligned
    return None


def build_rotation_layers(loop_buffer, n_layers, seed=-1):
    """
    Generate n_layers circular rotations of loop_buffer. Layer 0 always
    keeps offset 0 (the untouched reference loop, at the original phase);
    layers 1..n_layers-1 get random offsets -- so the "additional layers"
    language in compute_layer_gains' falloff scheme lines up with the
    layers that are actually rotated away from the source.

    seed=-1 -> nondeterministic (fresh OS entropy each call, a different
    stack every render). Any other int -> reproducible offsets.

    Returns (offsets, layers): offsets is an int array of length n_layers
    (offsets[0] == 0); layers is a (n_layers, len(loop_buffer)) float array,
    where layers[i] = loop_buffer read starting from sample offsets[i]
    (wrapping), i.e. np.roll(loop_buffer, -offsets[i]).
    """
    if not 2 <= n_layers <= 100:
        raise ValueError(f"n_layers must be between 2 and 100, got {n_layers}")
    n = len(loop_buffer)
    rng = np.random.default_rng(None if seed == -1 else seed)
    offsets = np.zeros(n_layers, dtype=np.int64)
    offsets[1:] = rng.integers(0, n, size=n_layers - 1)
    layers = np.stack([np.roll(loop_buffer, -o) for o in offsets])
    return offsets, layers


def compute_layer_gains(n_layers, falloff_db=None):
    """
    Per-layer linear gain applied before summing.

    falloff_db=None (default, "auto"): equal-power split, 1/sqrt(n_layers)
    for every layer. Each layer is the same periodic waveform at a
    different rotation -- a harmonic at frequency f picks up a phase shift
    of 2*pi*f*offset/period that's effectively uniformly random per layer
    (random offset), so for loudness purposes the layers behave like
    decorrelated sources: n copies each at 1/sqrt(n) sum to roughly the
    same RMS as one copy at unity gain, regardless of n_layers. More layers
    then thickens the timbre (more comb-filter ripple between the phase
    relationships) without changing the overall loudness.

    falloff_db=value: manual alternative -- layer i (0-indexed) gets
    -i*value dB, so the reference layer (offset 0) stays at full level and
    each additional rotated layer is quieter than the last by a fixed
    amount (the "3-6 dB per extra layer" scheme).

    Either scheme is only an average-case estimate -- combine_layers()
    still applies a final safety peak check afterwards, since an unlucky
    seed can still land several layers' low harmonics in phase and produce
    a louder peak than the estimate predicts.
    """
    if falloff_db is None:
        return np.full(n_layers, 1.0 / np.sqrt(n_layers))
    return 10 ** (-np.arange(n_layers) * falloff_db / 20)


def combine_layers(layers, gains, reference_peak):
    """
    Weighted-sum the (n_layers, period) rotated layers into one period, then
    a safety-only peak normalization against `reference_peak` (the original
    single-layer loop's peak): attenuates if the weighted sum happens to
    exceed it, never boosts. Guards against a run of bad luck in the random
    rotation offsets clipping the result, regardless of which gain scheme
    compute_layer_gains used.
    """
    combined = np.sum(layers * gains[:, None], axis=0)
    peak = np.max(np.abs(combined))
    if peak > reference_peak and peak > 0:
        combined *= reference_peak / peak
        print(f"[combine_layers] safety normalize: peak {peak:.4f} -> {reference_peak:.4f} "
              f"(rotation offsets briefly lined up in phase)")
    return combined


def build_rotation_loop(x, sr, loop_start, loop_end, n_layers=8, seed=-1, falloff_db=None,
                         xfade_ms=30.0, xfade_curve=0.5):
    """
    Build a Rotation Loop from the file's own [loop_start, loop_end) region:
    n_layers circular rotations of that region, summed into a single
    thicker loop of the same period, then crossfaded onto the real attack.

    Because circular rotation and summation commute (roll(sum(layers), s)
    == sum(roll(layer, s) for each layer)), rotating the whole COMBINED
    buffer to align with the real attack -- same _circular_best_shift
    technique as freeze_loop.build_seamless_loop / wavetable_extract's
    attach_attack -- is equivalent to rotating every individual layer by
    that same shift. It does not disturb the relative offsets between
    layers that build_rotation_layers set up; it only repositions where the
    tiled combined loop starts relative to the attack.

    The source loop is assumed already seamless (a precondition of this
    method, not something this function verifies): rotating a seamless
    periodic buffer keeps it seamless (the wrap point just moves), and
    summing several such buffers of identical length gives another exactly
    periodic buffer -- so no extra seam correction is needed at the
    combined loop's own wrap, only the attack-side splice matters.

    Returns (attack, loop_buffer, xfade_n) -- same contract as
    freeze_loop.build_seamless_loop: attack already has loop_buffer[:xfade_n]
    blended into its tail; resume playback at loop_buffer[xfade_n:], then
    tile the full loop_buffer (np.tile) for indefinite sustain.
    """
    if loop_end <= loop_start:
        raise ValueError(f"invalid loop region: start={loop_start} end={loop_end}")

    loop_buffer = x[loop_start:loop_end].astype(np.float64)
    reference_peak = np.max(np.abs(loop_buffer))

    offsets, layers = build_rotation_layers(loop_buffer, n_layers, seed=seed)
    gains = compute_layer_gains(n_layers, falloff_db=falloff_db)
    combined = combine_layers(layers, gains, reference_peak)
    print(f"[build_rotation_loop] {n_layers} layers, seed={seed}, offsets={offsets.tolist()}, "
          f"gain scheme={'auto (equal-power)' if falloff_db is None else f'{falloff_db} dB/layer'}")

    xfade_n = _resolve_xfade_n(sr, xfade_ms, {
        "pre-loop audio (loop_start)": loop_start,
        "loop region": len(combined),
    })
    reference = x[loop_start - xfade_n: loop_start].astype(np.float64)

    shift = _circular_best_shift(combined, reference)
    combined = np.roll(combined, -shift)

    fade_out, fade_in = _crossfade_gains(xfade_n, xfade_curve)
    crossfaded = reference * fade_out + combined[:xfade_n] * fade_in

    attack = np.concatenate([x[:loop_start - xfade_n], crossfaded]).astype(np.float32)
    return attack, combined.astype(np.float32), xfade_n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="input wav file -- MUST already contain a defined loop (smpl chunk), assumed seamless")
    ap.add_argument("-o", "--output", default="rotation_out.wav")
    ap.add_argument("--n-layers", type=int, default=8, help="number of rotated layers, 2-100 (default 8)")
    ap.add_argument("--seed", type=int, default=-1, help="-1 = random offsets every run (default); any other int = reproducible")
    ap.add_argument("--layer-falloff-db", type=float, default=None,
                     help="manual per-additional-layer attenuation in dB (e.g. 3-6). Omit for automatic "
                          "equal-power leveling (default), which keeps loudness ~constant regardless of --n-layers")
    ap.add_argument("--loop-start", type=int, default=None, help="override loop start, in samples (default: read from the file's smpl chunk)")
    ap.add_argument("--loop-end", type=int, default=None, help="override loop end, exclusive, in samples (default: read from the file's smpl chunk)")
    ap.add_argument("--xfade-ms", type=float, default=30.0, help="crossfade length from the real attack into the rotation loop, in ms")
    ap.add_argument("--xfade-curve", type=float, default=0.5, help="0=linear, 1=equal-power (default: blend, the rotation loop is aligned to the attack)")
    ap.add_argument("--root-note", type=int, default=None, help="MIDI unity note for the output smpl chunk (default: preserve the source file's, or 60/C4)")
    ap.add_argument("--no-smpl", action="store_true", help="skip writing the smpl loop chunk to the output")
    args = ap.parse_args()

    if not 2 <= args.n_layers <= 100:
        ap.error(f"--n-layers must be between 2 and 100, got {args.n_layers}")

    x, sr = sf.read(args.input, always_2d=False)
    if x.ndim > 1:
        x = x.mean(axis=1)

    smpl = read_smpl_loop(args.input)
    if args.loop_start is not None and args.loop_end is not None:
        loop_start, loop_end = args.loop_start, args.loop_end
        root_note = args.root_note if args.root_note is not None else (smpl[2] if smpl else 60)
    elif smpl is not None:
        loop_start, loop_end, file_root_note = smpl
        root_note = args.root_note if args.root_note is not None else file_root_note
    else:
        ap.error(f"{args.input} has no smpl loop chunk -- Rotation Loop requires a file with an "
                 f"already-defined, seamless loop. Pass --loop-start/--loop-end to override.")
        return

    attack, loop_buffer, xfade_n = build_rotation_loop(
        x, sr, loop_start, loop_end,
        n_layers=args.n_layers, seed=args.seed, falloff_db=args.layer_falloff_db,
        xfade_ms=args.xfade_ms, xfade_curve=args.xfade_curve,
    )

    # attack already contains loop_buffer[:xfade_n] (blended); append just
    # the rest of that one period -- no tiling. The output holds exactly one
    # loop period, and the smpl chunk below is what tells a sampler/DAW to
    # repeat [new_loop_start, new_loop_end) indefinitely at playback; the
    # file itself doesn't need to contain multiple copies of it.
    out = np.concatenate([attack, loop_buffer[xfade_n:]]).astype(np.float32)
    sf.write(args.output, out, sr)
    print(f"rotation loop: {args.n_layers} layers, seed={args.seed}, period={len(loop_buffer) / sr * 1000:.2f}ms "
          f"-> wrote {args.output} ({len(out) / sr:.2f}s, peak={np.max(np.abs(out)):.3f})")

    if not args.no_smpl:
        new_loop_start = len(attack)
        new_loop_end = new_loop_start + len(loop_buffer)
        write_smpl_loop(args.output, sr, new_loop_start, new_loop_end, root_note=root_note)


if __name__ == "__main__":
    main()
