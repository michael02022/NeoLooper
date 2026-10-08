// Audio document: the loaded sample, its metadata (loop, root note, tune),
// clipboard and undo/redo stack. All of it lives in RAM.
//
// Only the GUI thread and the host's main thread (state save/load) touch it,
// serialized with the plugin's mutex. The audio thread receives immutable
// snapshots through the Engine.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "audio_data.hpp"
#include "envelope.hpp"
#include "procmods/amplitude_dsp.hpp"
#include "procmods/filter_dsp.hpp"
#include "shared.hpp"

class Document {
public:
    // publish: called whenever the audio snapshot changes (so the Engine can
    // hand it to the audio thread).
    Document(SharedParams& params, std::function<void(AudioDataPtr)> publish);

    // --- access ---
    AudioDataPtr data() const { return cur_; }
    bool hasAudio() const { return cur_ && cur_->frames() > 0; }
    int64_t frames() const { return cur_ ? cur_->frames() : 0; }

    int64_t loopStart() const { return params_.loopStart.load(); }
    int64_t loopEnd() const { return params_.loopEnd.load(); }
    int64_t selStart() const { return params_.selStart.load(); }
    int64_t selEnd() const { return params_.selEnd.load(); }
    bool hasSelection() const { return selStart() >= 0 && selEnd() > selStart(); }
    bool hasLoop() const { return loopStart() >= 0 && loopEnd() > loopStart(); }

    // Effective loop region: markers if present, otherwise the selection.
    bool effectiveLoop(int64_t& s, int64_t& e) const;

    int rootNote() const { return params_.rootNote.load(); }
    float tuneCents() const { return params_.tuneCents.load(); }
    void setRootNote(int n);
    void setTuneCents(float c);

    void setSelection(int64_t s, int64_t e); // s<0 clears it
    void clearSelection() { setSelection(-1, -1); }
    void setLoopPoints(int64_t s, int64_t e, bool undoable);
    void clearLoopPoints(bool undoable);

    // --- loading / recording ---
    void setAudio(AudioDataPtr d, const char* opName); // undoable
    // Consumed by WaveformView: true exactly once right after setAudio()/
    // loadFromState() load a REALLY new sample (not an edit of the current
    // one), to force reframing to the full waveform instead of keeping
    // whatever zoom/scroll was active before. Resets to false once read.
    bool consumeFullViewRequest();
    // Appends audio to the end of the document (recording): adapts channels
    // and sample rate to the document; if there's no document yet, it
    // becomes it.
    void appendAudio(AudioDataPtr rec, const char* opName);
    // Base name (no extension) of the loaded sample, used to prefill the
    // export dialog. "" = defaults to "loop". Travels with every undo/redo
    // snapshot alongside the audio it represents.
    const std::string& sampleName() const { return sampleName_; }
    void setSampleName(const std::string& name) { sampleName_ = name; }
    void reset(); // empty document
    // Restores from the host's project state (no undo entry).
    void loadFromState(AudioDataPtr d, int64_t loopStart, int64_t loopEnd,
                       int rootNote, float tuneCents,
                       const std::string& sampleName);

    // --- envelopes being edited (non-destructive, audible and undoable) ---
    Envelopes env;
    // Undo snapshot before an envelope-point edit.
    void pushEnvUndo(const char* opName);

    // --- editing (all undoable) ---
    bool cutSelection();
    bool copySelection();
    bool deleteSelection();
    bool paste();
    bool hasClipboard() const { return clipboard_ != nullptr; }
    void normalize(); // with an active selection, normalizes ONLY the selection
    // Reverses the selection (or the whole sample if there's no selection),
    // sample by sample. Duration doesn't change, so loop start/end stay
    // exactly where they were.
    void reverse();
    // Inverts the polarity (multiplies by -1) of the selection, or the whole
    // sample if there's no selection (same criterion as reverse()).
    // channel = -1: every channel (the only channel of a mono sample, both
    // of a stereo one); 0/1: only L/R. Duration doesn't change, loop
    // start/end stay intact. No-op if the requested channel doesn't exist.
    void invertPolarity(int channel = -1);
    // Applies a high-pass + bell band (see procmods/filter.hpp) to the
    // selection, or to the whole sample if there's no selection -- time
    // domain filtering (biquads), without changing duration (loop start/end
    // stay intact, same criterion as reverse()). No-op if there's no audio or
    // the range is invalid.
    void applyFilter(const filterdsp::FilterSpec& spec);
    // Applies a plain gain (see procmods/amplitude.hpp) to the selection,
    // or to the whole sample if there's no selection -- same criterion,
    // and same in-place/no-marker-remap behavior, as applyFilter.
    void applyAmplitude(const amplitudedsp::AmplitudeSpec& spec);
    // Transposes (see procmods/pitch_method.hpp) the selection, or the
    // whole sample if there's no selection (same criterion as applyFilter/
    // reverse/normalize). 'render' is called with the document's CURRENT
    // audio and the freshly-recomputed [s0,s1) (same "recompute at Apply
    // time" rule as applyFilter/applyStretch), and must return the pitch-
    // shifted replacement for that range (one vector per channel; empty =
    // failed gracefully). Decoupled from any specific pitch algorithm on
    // purpose, same reasoning as applyStretch: several methods share this
    // one entry point (see PitchMethod::draw), each supplying its own
    // 'render'. The piece's length may differ from the input region (e.g.
    // the built-in method's traditional/non-duration-preserving mode) or
    // match it exactly (every other case) -- either way, marker remap is
    // handled the same way as applyStretch (see spliceAndRemap), which
    // degrades to a no-op shift when the length doesn't change.
    // methodName becomes the undo history entry and the toast text.
    // resetTune zeroes the live Tune (finetune) parameter, written AFTER
    // commit() so undo captures the OLD tuneCents_ (same ordering rule as
    // applyEnvelopes' marker remap). No-op if there's no audio, the range
    // is invalid, or render() fails.
    using PitchRenderFn = std::function<std::vector<std::vector<float>>(const AudioData&, int64_t, int64_t)>;
    void applyPitch(const PitchRenderFn& render, const char* methodName, bool resetTune);
    // Changes duration (see procmods/stretch_method.hpp) of the selection,
    // or the whole sample if there's no selection, WITHOUT changing pitch
    // -- the mirror image of applyPitch's traditional mode. 'render' is
    // called with the document's CURRENT audio and the freshly-recomputed
    // [s0,s1) (same "recompute at Apply time" rule as applyFilter/
    // applyPitch), and must return the stretched replacement for that
    // range (one vector per channel; empty = failed gracefully, e.g. an
    // engine error or a region too short for the method's minimum
    // window). Decoupled from any specific stretch algorithm on purpose:
    // several methods share this one entry point (see
    // StretchMethod::draw), each supplying its own 'render'.
    // methodName becomes the undo history entry and the toast text. Same
    // marker remap as applyPitch (see spliceAndRemap).
    // No-op if there's no audio, the range is invalid, or render() fails.
    using StretchRenderFn = std::function<std::vector<std::vector<float>>(const AudioData&, int64_t, int64_t)>;
    void applyStretch(const StretchRenderFn& render, const char* methodName);
    void toMono(int mode); // 0 = L channel, 1 = R channel, 2 = L+R mix
    void changeSampleRate(double sr, bool resample);
    // "Bakes" the current tune (cents) into the declared sample rate:
    // multiplies sampleRate by 2^(tuneCents/1200) WITHOUT resampling the
    // audio and sets tuneCents to 0. The playback ratio (sampleRate/hostSr,
    // cents aside) stays identical to before -- same pitch when played at
    // the root note, just baked into the sample rate instead of living in
    // the tune parameter. No-op if there's no audio or tune is already 0.
    void printTuneToSampleRate();
    void toggleBits(); // 16 <-> 32
    void applyEnvelopes();
    void clearAll();          // Delete with no selection: empty the editor (undoable)
    void trimAfterLoopEnd();    // deletes all audio after loop end
    void trimBeforeLoopStart(); // deletes all audio before loop start (remove the attack)
    // Duplicates the marked loop region [loopStart,loopEnd) and inserts the
    // copy right after it, extending loopEnd to the end of the new copy
    // (loopStart stays put): a one-cycle loop becomes a two-cycle loop of
    // the same audio. Markers at/after the insertion point (loopEnd) shift
    // forward by the loop's length, same rule as paste()'s marker shift.
    // No-op if there's no audio or no loop marked.
    void extendLoop();
    // Deletes leading/trailing silence: samples (any channel) whose
    // |amplitude| stays at or below the threshold (thresholdDb, e.g. -100
    // = only exact digital silence, -20 = fairly aggressive) from the very
    // start and the very end of the sample, keeping only the region in
    // between. Markers shift back by however much was removed from the
    // start (same clampMarkersToLength() invalidation rule as any other
    // edit if a marker falls inside the removed region). No-op if there's
    // no audio, the sample is fully silent, or there's nothing to trim.
    void trimSilence(float thresholdDb);
    // Replaces [start,end) with 'piece' (same channel count) and sets the
    // new loop points. Used by loop modules when applying.
    void replaceRange(int64_t start, int64_t end,
                      std::vector<std::vector<float>>&& piece,
                      int64_t newLoopStart, int64_t newLoopEnd,
                      const char* opName);
    // Same as replaceRange, but WITHOUT touching the loop/selection markers
    // (they're only clamped to the new length, same as any other edit). Used
    // by modules that don't require or define a loop -- e.g. Freeze, which
    // inserts a chunk at an arbitrary point in the document without assuming
    // anything about existing loop start/end.
    void insertPiece(int64_t start, int64_t end,
                     std::vector<std::vector<float>>&& piece,
                     const char* opName);

    // --- undo / redo ---
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

    // Ephemeral messages for the GUI ("Converted to 32-bit", etc.).
    std::string takeToast();

private:
    struct Snapshot {
        AudioDataPtr data;
        int64_t loopStart, loopEnd;
        int rootNote;
        float tuneCents;
        Envelopes env;
        std::string sampleName;
        std::string opName;
    };

    Snapshot makeSnapshot(const char* op) const;
    void restoreSnapshot(const Snapshot& s);
    void pushUndo(const char* opName);
    // Quantizes to 16 bits, or promotes to 32 if the amplitude doesn't fit.
    void finalizeFormat(AudioData& d);
    void commit(std::shared_ptr<AudioData> d, const char* opName,
                bool keepLoop = true);
    // Erases [s0,s1) and inserts 'piece' (one sub-vector per channel,
    // possibly a DIFFERENT length than s1-s0) in its place, then remaps
    // loop markers: untouched before s0, rescaled PROPORTIONALLY to
    // piece's new length when they fall inside [s0,s1) (same idea as
    // changeSampleRate's whole-document rescale, just scoped to the edited
    // span), and shifted by the length delta when they fall at/after s1
    // (same idea as paste()'s marker shift). Selection is set to exactly
    // the newly edited region. Shared by applyPitch's traditional mode and
    // applyStretch, the two edits whose region length can change.
    void spliceAndRemap(int64_t s0, int64_t s1, std::vector<std::vector<float>>&& piece,
                        const char* opName);
    void clampMarkersToLength();
    void toast(const std::string& msg);

    SharedParams& params_;
    std::function<void(AudioDataPtr)> publish_;
    AudioDataPtr cur_;
    AudioDataPtr clipboard_;
    std::string sampleName_;
    std::deque<Snapshot> undo_, redo_;
    std::string toast_;
    bool fullViewRequested_ = false;
    static constexpr size_t kMaxUndo = 48;
};
