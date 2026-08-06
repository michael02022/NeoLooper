"""
Mirror Loop (STFT edition): a ping-pong loop built by reversing the ORDER of
spectral frames, not the time-domain samples.

The first version of this script built the ping-pong effect by literally
reading the buffer backwards for half of each lap -- which is exactly the
"sounds reversed" quality it was trying to explain away, not reproduce
faithfully. This is a different construction: reuse freeze_loop.py's
phase-vocoder machinery (STFT, true-frequency estimation per bin) to
decouple two things that a literal time-reversal always couples together:

  - WHICH frame's magnitude spectrum plays at a given moment (the timbre/
    envelope's position in the source material) -- this is what gets
    reversed, i.e. walked backward through the recorded frame sequence.
  - How the SYNTHESIS PHASE advances from one output frame to the next --
    this always accumulates FORWARD, adding each frame's own measured true
    frequency (never negated), exactly like normal forward playback would.

Concretely: take the window's M STFT frames, each carrying (magnitude,
true_omega) per bin (true_omega estimated the same way build_seamless_loop
does). Build a frame-INDEX sequence [0, 1, ..., M-1, M-2, ..., 1] (forward
then backward, both endpoints kept once per lap -- same duplicate-avoidance
as the original mirror_loop's sample-domain version). At output step s, use
mag[sequence[s]] for magnitude and phase[s] = phase[s-1] + true_omega[
sequence[s]] * hop for phase -- i.e. the phase recurrence never runs
backward, even while `sequence` is walking down through the frame indices.

Since the oscillator phase itself is never reversed, the fine periodic
structure within each frame stays completely normal-sounding (no pitch
dip-then-recover, no reversed transient click); only the spectral envelope's
position in time -- which harmonic was loud when, where a formant or a sync
sweep was in its trajectory -- plays backward. That's "inverting the wave's
time, not the wave" concretely, not just a description.

Per-bin phase alone isn't sufficient, though (first version of this file
verified that by direct waveform inspection, not just by trusting the math):
a sawtooth's sharp edge depends on ALL its harmonics sitting in one precise
RELATIVE phase alignment, not just each one individually having a sane
frequency. Accumulating every bin's phase independently lets that alignment
drift apart the moment frame order stops being strictly forward -- non-peak
bins (window-sidelobe leakage around each harmonic) carry an especially
noisy true-frequency estimate, and with nothing anchoring them to their
harmonic's peak, they wander and blur the edge into a smeared "U" shape
instead of a mirrored ramp. The fix is identity phase locking (Laroche &
Dolson): find each harmonic's peak bin, let ONLY the peak accumulate phase
freely (via true_omega, as above), and lock every other bin in its
neighborhood to that peak's phase trajectory plus the bin's ORIGINAL
measured phase offset from the peak in whichever frame is currently
selected. That keeps each harmonic's local shape rigid -- and hence the
sawtooth's edge intact -- while still letting the overall envelope position
(which peak's phase story we're following) walk backward through the frame
sequence.

This will do very little on a stationary tone (a steady sawtooth's envelope
barely changes frame to frame, so reversing frame order is nearly a no-op --
useful as a "does this introduce artifacts on its own" sanity check, but not
where the effect is audible). It needs a window with real spectral motion
(a decay, a sync sweep, a formant) to have anything to reverse -- try
sawtoothloop.wav's own loop region, or piano1.wav/strings1.wav.

--fft-size matters a lot more than usual for periodic-transient material
like a hard-synced sawtooth (verified directly: the default 2048 -- ~12
fundamental periods of sawtoothloop.wav's ~169-sample period per analysis
window -- reconstructed the backward pass as a smeared "U" shape even WITH
phase locking; dropping to --fft-size 512 --hop 128, ~3 periods per window,
fixed it -- the backward pass came back out as genuine ascending ramps with
resets, same character as the source). The analysis window has to be short
enough that "one frame's spectral snapshot" still corresponds to roughly
one cycle's worth of the fast structure you're trying to preserve; a window
spanning a dozen reset cycles just describes their AVERAGE spectral energy,
which identity phase locking can't turn back into the individual edges. For
slowly-evolving material (piano/strings decay, formant motion) the default
is fine since there's no fast periodic structure being averaged away.

Like freeze_loop.py's build_seamless_loop, one lap is snapped so its total
phase advance is an exact multiple of 2*pi per bin -- see
synthesize_frame_sequence's docstring -- so the rendered smpl region tiles
indefinitely with no drift or beating, and can be handed to a sampler as a
single repeating region instead of needing --laps repeats baked into the
file. --laps still renders multiple copies directly for convenient
auditioning without a sampler.

Usage:
    python mirror_loop.py sawtoothloop.wav --loop-start 65909 --loop-end 76041 --laps 4 --plot
    python mirror_loop.py sawtooth_clean.wav --loop-start 2000 --window-ms 300 --compare-forward --plot
"""
import argparse

import numpy as np
import soundfile as sf
from scipy.signal.windows import hann

from freeze_loop import _crossfade_gains, _resolve_xfade_n, istft, princarg, stft, write_smpl_loop
from rotation_loop import read_smpl_loop


def estimate_frame_frequencies(X, hop, n_fft):
    """
    X: STFT frames (n_frames, n_bins), where frame 0 is one hop of CONTEXT
    before the window of interest (needed as the phase reference for frame
    1's true-frequency estimate) and frames 1..n_frames-1 are the real
    window.

    Returns (mag, omega_true, phase_raw):
      mag        - |X[1:]|, shape (n_frames-1, n_bins)
      omega_true - per-bin true instantaneous frequency (rad/sample) for
                    each of those frames, phase-vocoder estimate exactly
                    like freeze_loop.build_seamless_loop uses at a single
                    frame, just computed for every frame instead of one
      phase_raw  - angle(X[1:]), shape (n_frames-1, n_bins) -- the real
                    measured phase of every frame (not just the first), kept
                    around so identity phase locking can read each bin's
                    ORIGINAL phase offset from its harmonic's peak in
                    whichever source frame ends up selected
    """
    n_frames, n_bins = X.shape
    k = np.arange(n_bins)
    omega_nominal = 2 * np.pi * k / n_fft
    phase = np.angle(X)
    mag = np.abs(X)
    expected = omega_nominal * hop
    deviation = princarg(phase[1:] - phase[:-1] - expected[None, :])
    omega_true = omega_nominal[None, :] + deviation / hop
    return mag[1:], omega_true, phase[1:]


def find_spectral_peaks(mag, min_bin_distance=3, height_ratio=0.02):
    """
    Peaks are found on the frame-AVERAGED magnitude spectrum (mag.mean(0)),
    not re-detected every frame -- a stable harmonic tone's peak bins barely
    move within one loop window, and keeping peak identity fixed is what
    lets synthesize_frame_sequence() track "the same peak's" phase
    continuously across the (reordered) frame sequence. min_bin_distance
    avoids picking multiple bins off the same harmonic's mainlobe;
    height_ratio ignores noise-floor bumps relative to the tallest peak.

    Returns (peak_bins, region_of): peak_bins is the array of peak bin
    indices; region_of has length n_bins, mapping every bin to the INDEX
    (into peak_bins, not the bin number itself) of its nearest peak.
    Falls back to "every bin is its own peak" (no locking at all) if no
    peak clears the threshold, so callers degrade gracefully instead of
    crashing on unusual/very quiet input.
    """
    from scipy.signal import find_peaks

    avg_mag = mag.mean(axis=0)
    peaks, _ = find_peaks(avg_mag, distance=min_bin_distance, height=avg_mag.max() * height_ratio)
    if len(peaks) == 0:
        n_bins = mag.shape[1]
        return np.arange(n_bins), np.arange(n_bins)
    bins = np.arange(mag.shape[1])
    region_of = np.abs(bins[:, None] - peaks[None, :]).argmin(axis=1)
    return peaks, region_of


def mirror_frame_sequence(n_frames):
    """
    [0, 1, ..., n_frames-1, n_frames-2, ..., 1] -- forward then backward,
    with frame 0 and frame n_frames-1 each appearing once per lap (dropped
    from whichever half would otherwise repeat them), same reasoning as the
    sample-domain mirror buffer: no frame index doubles up at either turn.
    """
    if n_frames < 2:
        raise ValueError(f"need at least 2 STFT frames to mirror, got {n_frames}")
    forward = np.arange(n_frames)
    backward = np.arange(n_frames - 2, 0, -1)
    return np.concatenate([forward, backward])


def synthesize_frame_sequence(mag, omega_true, phase_raw, peak_bins, region_of, sequence, n_laps, n_fft, hop, win):
    """
    Render `n_laps` repeats of `sequence` (an array of frame indices into
    mag/omega_true/phase_raw). Magnitude follows the sequence directly.
    Phase is identity-phase-locked (see module docstring):

      1. Peak bins accumulate phase FORWARD using their own true_omega,
         never negated, regardless of which direction `sequence` is
         currently walking through frame indices -- same trick as before,
         now applied only to the (much more reliable) peak bins.
      2. Every other bin is locked to its region's peak: phase[bin] =
         phase[that peak] + (phase_raw[bin] - phase_raw[peak]) using the
         CURRENTLY SELECTED source frame's raw measured offset -- rigid
         relative to the peak, not independently accumulated.

    Synthesizes pad_frames extra steps on both sides (same reasoning as
    build_seamless_loop: keeps the extracted region interior to the
    overlap-add, unaffected by the true array edges) via one vectorized
    cumulative sum anchored at frame 0's measured peak phase, then slices
    out just the requested n_laps.

    Before that, each peak bin's true_omega is snapped to the nearest value
    that makes ONE LAP's total phase advance an exact multiple of 2*pi --
    same quantization freeze_loop.build_seamless_loop applies to a single
    frozen frequency, generalized here to a per-frame value: sum omega_true
    over every frame `sequence` visits in one lap (a frame that appears in
    both the forward and backward halves counts twice, naturally, since it
    contributes phase advance twice per lap), round that to the nearest
    multiple of 2*pi, and rescale every frame's omega for that bin by the
    (tiny, inaudible) ratio needed to hit it exactly. Locked (non-peak) bins
    need no separate quantization: their phase is always peak_phase + a
    per-frame offset that's already exactly periodic in `sequence`, so once
    the peaks close up cleanly every lap, everything locked to them does
    too. The result: one lap is bit-exact periodic, so tiling it (whether
    --laps here or an external sampler looping just the smpl region) never
    drifts or beats, and the loop is entirely self-contained synthesis --
    the crossfade at the attack splice is the only place real sample audio
    and synthesized audio ever mix.
    """
    L = len(sequence)
    total_steps = L * n_laps
    pad_frames = int(np.ceil(n_fft / hop)) + 1

    full_steps = np.arange(-pad_frames, total_steps + pad_frames)
    full_idx = sequence[full_steps % L]
    i0 = pad_frames  # position where full_steps[i0] == 0

    omega_peaks = omega_true[:, peak_bins]                    # (M, n_peaks)
    naive_lap_advance = omega_peaks[sequence].sum(axis=0) * hop        # (n_peaks,) -- total phase advance over one lap, unquantized
    n_cycles = np.round(naive_lap_advance / (2 * np.pi))
    safe_advance = np.where(np.abs(naive_lap_advance) < 1e-9, 1.0, naive_lap_advance)  # guard near-silent/DC peaks
    ratio = np.where(np.abs(naive_lap_advance) < 1e-9, 1.0, (n_cycles * 2 * np.pi) / safe_advance)
    omega_peaks = omega_peaks * ratio[None, :]

    phase_peak0 = phase_raw[0, peak_bins]                     # (n_peaks,)
    increments_peak = omega_peaks[full_idx] * hop              # (n_steps, n_peaks)
    cumsum_peak = np.cumsum(increments_peak, axis=0)
    phase_peak_full = phase_peak0[None, :] + (cumsum_peak - cumsum_peak[i0])   # (n_steps, n_peaks)

    peak_bin_for_bin = peak_bins[region_of]                    # (n_bins,)
    offset_all = phase_raw - phase_raw[:, peak_bin_for_bin]    # (M, n_bins) -- each bin's original offset from its peak
    offset_full = offset_all[full_idx]                         # (n_steps, n_bins)
    phase_full = phase_peak_full[:, region_of] + offset_full

    mag_full = mag[full_idx]

    Xf = mag_full * np.exp(1j * phase_full)
    y_full = istft(Xf, n_fft, hop, win)

    start = pad_frames * hop
    return y_full[start: start + total_steps * hop]


def build_mirror_loop(x, sr, loop_start, loop_end, n_fft=2048, hop=None, n_laps=6,
                       xfade_ms=20.0, xfade_curve=0.5, forward_only=False, phase_lock=True):
    """
    Returns (attack, body, xfade_n): attack is x up to the splice (tail
    already crossfaded into body's first xfade_n samples); body is n_laps
    laps of forward(+backward, unless forward_only) frame-sequence synthesis,
    already including the crossfaded head -- concatenate([attack,
    body[xfade_n:]]) for the full render, same contract shape as this
    project's other build_*_loop functions even though body isn't a single
    tileable period here (see module docstring's periodicity caveat).
    """
    if hop is None:
        hop = n_fft // 4
    if loop_end <= loop_start:
        raise ValueError(f"invalid loop region: start={loop_start} end={loop_end}")
    if loop_start < hop:
        raise ValueError(f"loop_start ({loop_start}) must be >= hop ({hop}) samples, "
                          f"to leave room for one frame of phase-reference context before the window")

    win = hann(n_fft, sym=False)
    X = stft(x[loop_start - hop: loop_end], n_fft, hop, win)
    if X.shape[0] < 3:
        raise ValueError(f"window too short for --fft-size {n_fft}: only {X.shape[0]} STFT frames "
                          f"(need >= 3). Increase --window-ms or decrease --fft-size/--hop")

    mag, omega_true, phase_raw = estimate_frame_frequencies(X, hop, n_fft)
    n_frames = mag.shape[0]

    if phase_lock:
        peak_bins, region_of = find_spectral_peaks(mag)
    else:
        peak_bins, region_of = np.arange(mag.shape[1]), np.arange(mag.shape[1])

    sequence = np.arange(n_frames) if forward_only else mirror_frame_sequence(n_frames)
    body = synthesize_frame_sequence(mag, omega_true, phase_raw, peak_bins, region_of, sequence, n_laps, n_fft, hop, win)

    xfade_n = _resolve_xfade_n(sr, xfade_ms, {
        "pre-loop audio (loop_start)": loop_start,
        "rendered body": len(body),
    })
    reference = x[loop_start - xfade_n: loop_start].astype(np.float64)
    fade_out, fade_in = _crossfade_gains(xfade_n, xfade_curve)
    crossfaded = reference * fade_out + body[:xfade_n] * fade_in

    attack = np.concatenate([x[:loop_start - xfade_n], crossfaded]).astype(np.float32)
    return attack, body.astype(np.float32), xfade_n, len(sequence)


def plot_comparison(x, out_mirror, out_forward, sr, loop_start, loop_end, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(3, 1, figsize=(12, 9))
    axes[0].specgram(x, NFFT=1024, Fs=sr, noverlap=768)
    axes[0].axvspan(loop_start / sr, loop_end / sr, color="white", alpha=0.15)
    axes[0].set_title("input (shaded = mirror window)")
    axes[0].set_ylim(0, 4000)

    axes[1].specgram(out_mirror, NFFT=1024, Fs=sr, noverlap=768)
    axes[1].set_title("mirror (frame order reversed, phase always forward)")
    axes[1].set_ylim(0, 4000)

    if out_forward is not None:
        axes[2].specgram(out_forward, NFFT=1024, Fs=sr, noverlap=768)
        axes[2].set_title("A/B: forward-only frame sequence, same synthesis pipeline")
        axes[2].set_ylim(0, 4000)
    else:
        axes[2].axis("off")

    fig.tight_layout()
    fig.savefig(path, dpi=130)
    print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="input wav file")
    ap.add_argument("-o", "--output", default="mirror_out.wav")
    ap.add_argument("--loop-start", type=int, default=None,
                     help="window start, in samples (default: read from the file's smpl chunk)")
    ap.add_argument("--loop-end", type=int, default=None,
                     help="window end, exclusive, in samples (default: loop-start + --window-ms)")
    ap.add_argument("--window-ms", type=float, default=250.0,
                     help="window length in ms, used when --loop-end is not given (default 250ms -- "
                          "needs to span enough STFT frames for the frame-reversal to have something to do)")
    ap.add_argument("--fft-size", type=int, default=2048)
    ap.add_argument("--hop", type=int, default=None)
    ap.add_argument("--laps", type=int, default=6, help="how many forward+backward laps to render")
    ap.add_argument("--xfade-ms", type=float, default=20.0, help="crossfade length from the real attack into the body, in ms")
    ap.add_argument("--xfade-curve", type=float, default=0.5, help="0=linear, 1=equal-power")
    ap.add_argument("--root-note", type=int, default=None, help="MIDI unity note for the smpl chunk (default: preserve source's, or 60/C4)")
    ap.add_argument("--no-smpl", action="store_true", help="skip writing the smpl loop chunk")
    ap.add_argument("--compare-forward", action="store_true",
                     help="also render forward-only (no frame reversal) through the SAME synthesis pipeline, as <output stem>.forward.wav, for a fair A/B")
    ap.add_argument("--no-phase-lock", action="store_true",
                     help="disable identity phase locking (revert to per-bin independent phase accumulation) -- A/B reference to hear why locking matters")
    ap.add_argument("--plot", action="store_true", help="save a spectrogram comparison PNG")
    args = ap.parse_args()

    if args.hop is None:
        args.hop = args.fft_size // 4

    x, sr = sf.read(args.input, always_2d=False)
    if x.ndim > 1:
        x = x.mean(axis=1)

    if args.loop_start is not None:
        loop_start = args.loop_start
        loop_end = args.loop_end if args.loop_end is not None else loop_start + int(round(args.window_ms * sr / 1000))
        root_note = args.root_note if args.root_note is not None else 60
    else:
        smpl = read_smpl_loop(args.input)
        if smpl is None:
            ap.error(f"{args.input} has no smpl loop chunk -- pass --loop-start (and optionally --loop-end/--window-ms)")
            return
        loop_start, loop_end, file_root_note = smpl
        if args.loop_end is not None:
            loop_end = args.loop_end
        root_note = args.root_note if args.root_note is not None else file_root_note

    if loop_end <= loop_start or loop_end > len(x):
        ap.error(f"invalid window [{loop_start}:{loop_end}) for input of length {len(x)}")
        return

    attack, body, xfade_n, lap_frames = build_mirror_loop(
        x, sr, loop_start, loop_end, n_fft=args.fft_size, hop=args.hop, n_laps=args.laps,
        xfade_ms=args.xfade_ms, xfade_curve=args.xfade_curve, phase_lock=not args.no_phase_lock,
    )
    out = np.concatenate([attack, body[xfade_n:]]).astype(np.float32)
    sf.write(args.output, out, sr)
    print(f"mirror loop: window=[{loop_start}:{loop_end}) ({loop_end - loop_start} samples), "
          f"{lap_frames} STFT frames/lap, {args.laps} laps -> wrote {args.output} "
          f"({len(out) / sr:.2f}s, peak={np.max(np.abs(out)):.3f})")

    if not args.no_smpl:
        lap_hop_len = (lap_frames * args.hop)
        write_smpl_loop(args.output, sr, len(attack), len(attack) + lap_hop_len, root_note=root_note)
        print("[smpl] one lap is phase-quantized to close exactly (see module docstring) -- a sampler can "
              "tile just this region indefinitely with no drift/beating")

    out_forward = None
    if args.compare_forward:
        attack_f, body_f, xfade_n_f, _ = build_mirror_loop(
            x, sr, loop_start, loop_end, n_fft=args.fft_size, hop=args.hop, n_laps=args.laps,
            xfade_ms=args.xfade_ms, xfade_curve=args.xfade_curve, forward_only=True, phase_lock=not args.no_phase_lock,
        )
        out_forward = np.concatenate([attack_f, body_f[xfade_n_f:]]).astype(np.float32)
        fwd_path = args.output.rsplit(".", 1)[0] + ".forward.wav"
        sf.write(fwd_path, out_forward, sr)
        print(f"A/B reference (forward-only, same STFT pipeline) -> wrote {fwd_path}")

    if args.plot:
        plot_comparison(x, out, out_forward, sr, loop_start, loop_end, args.output + ".png")


if __name__ == "__main__":
    main()
