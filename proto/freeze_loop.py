"""
Spectral freeze / loop prototype.

Two related techniques, both built on quantizing each STFT bin's true
instantaneous frequency (phase-vocoder estimate) to an exact integer number
of cycles within a chosen duration T. That forces every bin back to its
exact starting phase after T seconds, which buys two things at once:

  1. Internal coherence: a real partial leaks (via window sidelobes) into
     several neighboring bins. If each bin held its own independently
     estimated frequency, they'd drift out of sync with each other over
     time and beat -- the classic phase-vocoder "detuned/chorused" artifact.
     Locking them all to a common harmonic grid (multiples of 1/T) keeps
     them coherent.
  2. Exact periodicity: the synthesized T-second block is a genuine single
     period, like a wavetable cycle -- not just spliced to sound okay. Any
     circular rotation of it is an equally valid loop start, so the entry
     point can be chosen (via circular cross-correlation against the real
     signal) to line up with the sample's own waveform for a clean
     crossfade, instead of an abrupt handoff.

build_seamless_loop() is the primitive: quantize + synthesize one exact
period T = loop_duration, entered via a phase-aligned crossfade from the
real attack. Meant to be tiled (np.tile) for indefinite sustain.

spectral_freeze() is pause/hold/resume: it calls build_seamless_loop() with
T = freeze_duration for the hold itself (so the same coherence + phase-
aligned entry apply), then crossfades back out into the original signal's
continuation -- that exit side has no such alignment available (the hold's
ending phase and the resumed original are unrelated points in time), so it
stays a plain equal-power blend. method="naive" instead holds one literal
complex frame with no phase advance at all, kept as an A/B reference for how
bad that sounds (overlap-adding identical frames periodic in `hop`, not in
the source's true pitch -> audible buzz).

Both write an `smpl` RIFF loop chunk (write_smpl_loop) marking the exact
periodic region, so a sampler/DAW can loop it indefinitely instead of
relying on however many periods got rendered into the file.

Usage:
    python freeze_loop.py freeze                                   # demo tone, pause/hold/resume
    python freeze_loop.py freeze in.wav -o out.wav --freeze-time 1.2 --freeze-duration 3.0
    python freeze_loop.py freeze --method naive                    # A/B against the buzzy version
    python freeze_loop.py loop in.wav -o loop.wav --freeze-time 1.2 --loop-duration 2.0
"""
import argparse

import numpy as np
import struct

import soundfile as sf
from scipy.signal.windows import hann


def princarg(phase):
    """Wrap phase (radians) to (-pi, pi]."""
    return (phase + np.pi) % (2 * np.pi) - np.pi


def stft(x, n_fft, hop, win):
    """Real STFT. Returns complex array of shape (n_frames, n_fft//2 + 1)."""
    n_frames = max(0, 1 + (len(x) - n_fft) // hop)
    X = np.empty((n_frames, n_fft // 2 + 1), dtype=np.complex128)
    for m in range(n_frames):
        frame = x[m * hop: m * hop + n_fft] * win
        X[m] = np.fft.rfft(frame)
    return X


def istft(X, n_fft, hop, win):
    """Windowed overlap-add synthesis, normalized by the sum of squared windows."""
    n_frames = X.shape[0]
    out_len = (n_frames - 1) * hop + n_fft
    y = np.zeros(out_len)
    norm = np.zeros(out_len)
    win2 = win ** 2
    for m in range(n_frames):
        frame = np.fft.irfft(X[m], n=n_fft) * win
        y[m * hop: m * hop + n_fft] += frame
        norm[m * hop: m * hop + n_fft] += win2
    norm[norm < 1e-8] = 1e-8
    return y / norm


def _resolve_xfade_n(sr, xfade_ms, caps):
    """
    Convert xfade_ms to samples, clamped to whatever material is actually
    available (caps: {label: n_samples available}). Prints a warning when
    the clamp changes the result, since a silently-shortened crossfade is
    easy to mistake for a bug (e.g. asking for more crossfade than there is
    audio before the freeze point silently eats the entire attack).
    """
    requested = int(sr * xfade_ms / 1000)
    n = min([requested] + list(caps.values()))
    if n < requested:
        binding, cap = min(caps.items(), key=lambda kv: kv[1])
        print(f"[warning] requested crossfade {xfade_ms:.1f}ms ({requested} samples) doesn't fit -- "
              f"clamped to {n} samples ({n / sr * 1000:.1f}ms), limited by '{binding}' "
              f"({cap} samples available)")
    return n


def _crossfade_gains(n, curve=1.0):
    """
    Fade-out/fade-in gains for an n-sample crossfade, blended between two
    families:
      curve=0 -> linear:       fade_out + fade_in       == 1 always.
                 Correct when both sides are phase-correlated/in-phase --
                 summing them stays at unity gain, no bump.
      curve=1 -> equal-power:  fade_out**2 + fade_in**2  == 1 always.
                 Correct when both sides are uncorrelated -- keeps perceived
                 loudness constant. Crossfading two IN-PHASE identical
                 signals with this curve instead produces a +3dB bump at the
                 midpoint (cos(pi/4) + sin(pi/4) = sqrt(2)).
    Values in between blend the two curves.
    """
    t = np.linspace(0, 1, n)
    lin_out, lin_in = 1 - t, t
    pow_out, pow_in = np.cos(t * np.pi / 2), np.sin(t * np.pi / 2)
    fade_out = (1 - curve) * lin_out + curve * pow_out
    fade_in = (1 - curve) * lin_in + curve * pow_in
    return fade_out, fade_in


def spectral_freeze(x, sr, freeze_time, freeze_duration, n_fft=2048, hop=None,
                     entry_xfade_ms=30.0, entry_xfade_curve=0.5,
                     exit_xfade_ms=25.0, exit_xfade_curve=1.0,
                     method="phase-vocoder"):
    """
    Pause `x` at `freeze_time`, hold it for `freeze_duration`, then let the
    original signal resume from where it paused.

    method="phase-vocoder" (recommended) delegates the hold itself to
    build_seamless_loop() with loop_duration=freeze_duration: every bin's
    frequency gets snapped to an exact integer multiple of 1/freeze_duration.
    That fixes the classic phase-vocoder "detuned/chorused" artifact: a real
    partial leaks (via window sidelobes) into several neighboring bins, and
    if each of those bins holds its own independently-estimated frequency,
    they drift out of sync with each other and beat. Quantizing locks them
    all to a common harmonic grid, so they stay coherent -- and as a side
    effect the hold becomes genuinely periodic, so the entry crossfade can
    use the same rotation-search alignment as loop mode instead of a hard
    cut, and the hold region can be smpl-loop-marked.

    method="naive" holds one literal complex frame with no phase advance at
    all -- the buzzy A/B reference (see module docstring). It's routed
    through the same build_seamless_loop() pipeline (with true_omega forced
    to 0 instead of estimated), so it still gets a rotation-aligned entry
    crossfade and a valid smpl loop region -- it just still buzzes, since
    quantizing a frequency that's already 0 is a no-op. Only phase-vocoder
    fixes the pitch itself.

    Returns (out, loop_start, loop_end): loop_start/loop_end mark the exact
    periodic hold region in `out`, for an smpl loop chunk.
    """
    if hop is None:
        hop = n_fft // 4
    freeze_start_sample = int(round(freeze_time * sr))

    attack, loop_buffer, entry_xfade_n = build_seamless_loop(
        x, sr, freeze_time, freeze_duration, n_fft=n_fft, hop=hop,
        xfade_ms=entry_xfade_ms, xfade_curve=entry_xfade_curve, method=method,
    )
    loop_start = len(attack)
    loop_end = loop_start + len(loop_buffer)
    held = np.concatenate([attack, loop_buffer[entry_xfade_n:]])

    post = x[freeze_start_sample:]
    exit_xfade_n = _resolve_xfade_n(sr, exit_xfade_ms, {
        "freeze_duration": len(held) - freeze_start_sample,
        "audio remaining after freeze_time": len(post),
    })
    if exit_xfade_n > 0:
        # held's ending phase and post's phase are unrelated (no alignment
        # search here, unlike the entry) -> equal-power (curve=1.0) is the
        # right default, not a bump-prone linear fade.
        fade_out, fade_in = _crossfade_gains(exit_xfade_n, exit_xfade_curve)
        splice = held[-exit_xfade_n:] * fade_out + post[:exit_xfade_n] * fade_in
        out = np.concatenate([held[:-exit_xfade_n], splice, post[exit_xfade_n:]])
    else:
        out = np.concatenate([held, post])

    return out.astype(np.float32), loop_start, loop_end


def _circular_best_shift(loop_buf, reference):
    """
    Find the circular shift `s` such that loop_buf[s : s+len(reference)]
    (wrapping) best matches `reference`, via FFT-based circular cross-
    correlation. Used to pick which point in a periodic loop to start
    playback at so it lines up with a real signal's waveform at a splice.
    """
    n = len(loop_buf)
    ref_padded = np.zeros(n)
    ref_padded[:len(reference)] = reference
    corr = np.fft.ifft(np.fft.fft(loop_buf) * np.conj(np.fft.fft(ref_padded))).real
    return int(np.argmax(corr))


def build_seamless_loop(x, sr, freeze_time, loop_duration, n_fft=2048, hop=None,
                         xfade_ms=30.0, xfade_curve=0.5, method="phase-vocoder"):
    """
    Build a spectrally-frozen loop that tiles with zero discontinuity at the
    wrap point (like a wavetable cycle): quantize each bin's true frequency
    to the nearest exact integer number of cycles within the loop length, so
    every partial returns to its exact starting phase after one period.
    Because the result is genuinely periodic, any circular rotation of it is
    an equally valid loop start -- so we search for the rotation whose
    waveform best matches the real signal right before the splice, and
    crossfade into that instead of an arbitrary (and likely mismatched)
    phase-0 start. Because that rotation search deliberately correlates the
    loop with the real signal, the crossfade defaults toward the linear end
    of _crossfade_gains (xfade_curve=0.5) rather than pure equal-power,
    which would risk a +3dB bump on the (partially) correlated overlap.

    method="naive" skips the phase-vocoder frequency estimate (true_omega=0
    for every bin), i.e. holds one literal complex frame with no phase
    advance -- the buzzy A/B reference (see module docstring). It still gets
    the same rotation-aligned entry crossfade and remains smpl-loop-markable
    (the held frame IS exactly periodic, just with period `hop`, unrelated
    to the source's actual pitch -- quantizing that against loop_duration is
    a no-op, since true_omega is already 0 everywhere, so it doesn't fix the
    buzz; only phase-vocoder addresses the frequency itself).

    Returns (attack, loop_buffer, xfade_n):
      attack       - x up to the splice point, tail already crossfaded into
                     the first xfade_n samples of loop_buffer
      loop_buffer  - one exact period; tile it (np.tile) for indefinite sustain
      xfade_n      - how many samples at the start of loop_buffer were
                     already played (blended) inside attack. Callers must
                     resume playback at loop_buffer[xfade_n:] and only start
                     tiling the *full* loop_buffer from the following period
                     -- restarting straight at loop_buffer[0] replays audio
                     that's already in attack and jumps to an unrelated
                     point in the waveform, which is audible as a click.
    """
    if hop is None:
        hop = n_fft // 4
    win = hann(n_fft, sym=False)

    X = stft(x, n_fft, hop, win)
    n_frames, n_bins = X.shape
    if n_frames < 2:
        raise ValueError("input too short for the given fft size")

    freeze_start_sample = int(round(freeze_time * sr))
    mf = int(round(freeze_start_sample / hop))
    mf = min(max(mf, 1), n_frames - 1)

    mag = np.abs(X[mf])
    phase_now = np.angle(X[mf])

    k = np.arange(n_bins)
    if method == "phase-vocoder":
        omega_nominal = 2 * np.pi * k / n_fft
        phase_prev = np.angle(X[mf - 1])
        expected = omega_nominal * hop
        deviation = princarg(phase_now - phase_prev - expected)
        true_omega = omega_nominal + deviation / hop
    elif method == "naive":
        true_omega = np.zeros(n_bins)   # phase never advances -> buzz at sr/hop
    else:
        raise ValueError(f"unknown method: {method}")

    n_loop_frames = max(1, int(round(loop_duration * sr / hop)))
    n_loop_samples = n_loop_frames * hop

    # snap every bin to an integer number of cycles over the loop length ->
    # phase(m + n_loop_frames) == phase(m) mod 2*pi for every bin, i.e. the
    # frame sequence itself becomes exactly periodic with period n_loop_frames
    n_cycles = np.round(true_omega * n_loop_samples / (2 * np.pi))
    quantized_omega = 2 * np.pi * n_cycles / n_loop_samples
    quantized_omega[0] = 0.0    # DC must stay real
    quantized_omega[-1] = 0.0   # Nyquist must stay real

    # synthesize extra frames on both sides so the extracted period is fully
    # "interior" to the overlap-add (unaffected by the true array edges)
    pad_frames = int(np.ceil(n_fft / hop)) + 1
    frame_idx = np.arange(-pad_frames, 2 * n_loop_frames + pad_frames)
    phases = phase_now[None, :] + frame_idx[:, None] * (quantized_omega[None, :] * hop)
    Xf = mag[None, :] * np.exp(1j * phases)
    y_full = istft(Xf, n_fft, hop, win)

    start = pad_frames * hop
    loop_buffer = y_full[start: start + n_loop_samples]

    next_period = y_full[start + n_loop_samples: start + 2 * n_loop_samples]
    seam_error = np.max(np.abs(loop_buffer - next_period))
    print(f"[build_seamless_loop] periodicity check: max |period[n] - period[n+1]| = {seam_error:.3e}")

    xfade_n = _resolve_xfade_n(sr, xfade_ms, {
        "pre-freeze audio (freeze_time)": freeze_start_sample,
        "loop_duration": n_loop_samples,
    })
    if xfade_n >= freeze_start_sample:
        print("[warning] crossfade consumes the entire pre-freeze signal -- 0 samples of unmodified "
              "attack remain before the blend into the loop starts. The real attack transient will "
              "audibly smear into the (stationary) loop almost from the start of the file. "
              "Reduce --xfade-ms or increase --freeze-time.")
    reference = x[freeze_start_sample - xfade_n: freeze_start_sample]

    shift = _circular_best_shift(loop_buffer, reference)
    loop_buffer = np.roll(loop_buffer, -shift)

    fade_out, fade_in = _crossfade_gains(xfade_n, xfade_curve)
    crossfaded = reference * fade_out + loop_buffer[:xfade_n] * fade_in

    attack = np.concatenate([x[:freeze_start_sample - xfade_n], crossfaded]).astype(np.float32)
    return attack, loop_buffer.astype(np.float32), xfade_n


def make_demo_signal(sr, duration=4.0, f0=220.0, seed=42):
    """A decaying plucked-like harmonic tone, so freezing mid-decay clearly
    demonstrates locking onto a still-evolving, non-stationary spectrum."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(sr * duration)) / sr
    y = np.zeros_like(t)
    for h in range(1, 7):
        freq = f0 * h * (1 + 0.0005 * rng.standard_normal())  # tiny natural detune
        decay = np.exp(-t * (0.12 * h))
        y += (1.0 / h) * np.sin(2 * np.pi * freq * t + rng.uniform(0, 2 * np.pi)) * decay
    y += 0.01 * rng.standard_normal(len(t))  # noise floor
    y /= np.max(np.abs(y)) * 1.05
    return y.astype(np.float32)


def plot_comparison(x, out, sr, freeze_time, freeze_duration, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 1, figsize=(10, 6), sharex=False)
    axes[0].specgram(x, NFFT=1024, Fs=sr, noverlap=768)
    axes[0].set_title("input")
    axes[0].set_ylim(0, 4000)

    axes[1].specgram(out, NFFT=1024, Fs=sr, noverlap=768)
    axes[1].axvspan(freeze_time, freeze_time + freeze_duration, color="white", alpha=0.15)
    axes[1].set_title("frozen output (shaded = frozen region)")
    axes[1].set_ylim(0, 4000)

    fig.tight_layout()
    fig.savefig(path, dpi=120)
    print(f"wrote {path}")


def write_smpl_loop(path, sr, loop_start, loop_end, root_note=60):
    """
    Append a `smpl` RIFF chunk to an already-written WAV file, encoding one
    forward loop over [loop_start, loop_end) samples (0-indexed, exclusive
    end -- same convention as everywhere else in this file).

    Gotcha (RIFF spec, confirmed by direct byte inspection): the chunk's
    dwEnd field is the index of the LAST looped sample, INCLUSIVE -- not
    exclusive like Python slicing. dwStart has no such offset. Appending the
    chunk after `data` (rather than inserting it before) is simpler and
    still spec-legal: RIFF readers must scan chunk IDs, not assume order.
    """
    dw_start = loop_start
    dw_end = loop_end - 1   # exclusive (our convention) -> inclusive (file format)

    sample_period_ns = int(round(1e9 / sr))
    header = struct.pack("<9I",
                          0,                  # manufacturer
                          0,                  # product
                          sample_period_ns,   # samplePeriod
                          root_note,          # midiUnityNote
                          0,                  # midiPitchFraction
                          0,                  # smpteFormat
                          0,                  # smpteOffset
                          1,                  # numSampleLoops
                          0)                  # samplerData (bytes following the loop table)
    loop = struct.pack("<6I",
                        0,        # cuePointID
                        0,        # type: 0 = loop forward
                        dw_start,
                        dw_end,
                        0,        # fraction
                        0)        # playCount, 0 = infinite
    body = header + loop
    chunk = b"smpl" + struct.pack("<I", len(body)) + body

    with open(path, "r+b") as f:
        f.seek(0, 2)
        f.write(chunk)
        file_size = f.tell()
        f.seek(4)
        f.write(struct.pack("<I", file_size - 8))  # RIFF chunk size = file size - 8

    print(f"[smpl] loop written: start={dw_start}, end={dw_end} (inclusive), "
          f"{dw_end - dw_start + 1} samples = {(dw_end - dw_start + 1) / sr * 1000:.1f}ms, "
          f"root_note={root_note}")


def _load_input(args):
    if args.input:
        x, sr = sf.read(args.input, always_2d=False)
        if x.ndim > 1:
            x = x.mean(axis=1)
    else:
        sr = 44100
        x = make_demo_signal(sr)
        print("no input given, using synthesized demo tone (decaying harmonic pluck)")
    return x, sr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    fz = sub.add_parser("freeze", help="pause the sample at a point, hold it, then resume")
    fz.add_argument("input", nargs="?", help="input wav file (omit to use a synthesized demo tone)")
    fz.add_argument("-o", "--output", default="freeze_out.wav")
    fz.add_argument("--freeze-time", type=float, default=1.5, help="seconds")
    fz.add_argument("--freeze-duration", type=float, default=3.0, help="seconds")
    fz.add_argument("--fft-size", type=int, default=2048)
    fz.add_argument("--hop", type=int, default=None)
    fz.add_argument("--entry-xfade-ms", type=float, default=30.0, help="crossfade length from the real attack into the hold, in ms")
    fz.add_argument("--entry-xfade-curve", type=float, default=0.5, help="0=linear, 1=equal-power (default: blend, hold is phase-aligned to the attack)")
    fz.add_argument("--exit-xfade-ms", type=float, default=25.0, help="crossfade length back into the resumed original, in ms")
    fz.add_argument("--exit-xfade-curve", type=float, default=1.0, help="0=linear, 1=equal-power (default: pure equal-power, hold/original are unaligned)")
    fz.add_argument("--method", choices=["phase-vocoder", "naive"], default="phase-vocoder")
    fz.add_argument("--root-note", type=int, default=60, help="MIDI unity note written to the smpl chunk (default 60 = C4)")
    fz.add_argument("--no-smpl", action="store_true", help="skip writing the smpl loop chunk")
    fz.add_argument("--plot", action="store_true", help="save a before/after spectrogram PNG")

    lp = sub.add_parser("loop", help="build a seamless, tileable loop (wavetable-style) from a freeze point")
    lp.add_argument("input", nargs="?", help="input wav file (omit to use a synthesized demo tone)")
    lp.add_argument("-o", "--output", default="loop_out.wav")
    lp.add_argument("--freeze-time", type=float, default=1.5, help="seconds")
    lp.add_argument("--loop-duration", type=float, default=2.0, help="seconds, one period")
    lp.add_argument("--loop-repeats", type=int, default=6, help="how many periods to render for the demo file")
    lp.add_argument("--fft-size", type=int, default=2048)
    lp.add_argument("--hop", type=int, default=None)
    lp.add_argument("--xfade-ms", type=float, default=30.0, help="crossfade length from the real attack into the loop, in ms")
    lp.add_argument("--xfade-curve", type=float, default=0.5, help="0=linear, 1=equal-power (default: blend, since the loop rotation is already correlated with the attack)")
    lp.add_argument("--root-note", type=int, default=60, help="MIDI unity note written to the smpl chunk (default 60 = C4)")
    lp.add_argument("--no-smpl", action="store_true", help="skip writing the smpl loop chunk")
    lp.add_argument("--plot", action="store_true", help="save a before/after spectrogram PNG")

    args = ap.parse_args()
    x, sr = _load_input(args)

    if args.cmd == "freeze":
        out, loop_start, loop_end = spectral_freeze(
            x, sr,
            freeze_time=args.freeze_time,
            freeze_duration=args.freeze_duration,
            n_fft=args.fft_size,
            hop=args.hop,
            entry_xfade_ms=args.entry_xfade_ms,
            entry_xfade_curve=args.entry_xfade_curve,
            exit_xfade_ms=args.exit_xfade_ms,
            exit_xfade_curve=args.exit_xfade_curve,
            method=args.method,
        )
        sf.write(args.output, out, sr)
        print(f"method={args.method}  freeze at {args.freeze_time}s for {args.freeze_duration}s "
              f"-> wrote {args.output} ({len(out)/sr:.2f}s, peak={np.max(np.abs(out)):.3f})")

        if args.no_smpl:
            pass
        elif loop_end > len(out):
            print(f"[warning] computed loop_end ({loop_end}) exceeds the rendered file length ({len(out)}) "
                  f"-- skipping smpl chunk (freeze_time too close to the end of the input?)")
        else:
            write_smpl_loop(args.output, sr, loop_start, loop_end, root_note=args.root_note)

        if args.plot:
            plot_comparison(x, out, sr, args.freeze_time, args.freeze_duration, args.output + ".png")

    elif args.cmd == "loop":
        attack, loop_buffer, xfade_n = build_seamless_loop(
            x, sr,
            freeze_time=args.freeze_time,
            loop_duration=args.loop_duration,
            n_fft=args.fft_size,
            hop=args.hop,
            xfade_ms=args.xfade_ms,
            xfade_curve=args.xfade_curve,
        )
        if not args.no_smpl and args.loop_repeats < 2:
            print(f"[warning] --loop-repeats {args.loop_repeats} is too low to contain one full clean loop "
                  f"period after the attack -- bumping to 2 so the smpl loop markers stay inside the file")
            args.loop_repeats = 2

        # attack already contains loop_buffer[:xfade_n] (blended); resume at
        # loop_buffer[xfade_n:] to finish that period, then tile full periods
        n_repeats = max(0, args.loop_repeats - 1)
        out = np.concatenate([attack, loop_buffer[xfade_n:], np.tile(loop_buffer, n_repeats)]).astype(np.float32)
        sf.write(args.output, out, sr)
        print(f"loop at {args.freeze_time}s, period={len(loop_buffer)/sr:.4f}s, "
              f"{args.loop_repeats} repeats -> wrote {args.output} ({len(out)/sr:.2f}s, peak={np.max(np.abs(out)):.3f})")

        if not args.no_smpl:
            loop_start = len(attack)
            loop_end = loop_start + len(loop_buffer)
            write_smpl_loop(args.output, sr, loop_start, loop_end, root_note=args.root_note)

        if args.plot:
            plot_comparison(x, out, sr, args.freeze_time, args.loop_duration * args.loop_repeats, args.output + ".png")


if __name__ == "__main__":
    main()
