#pragma once
// Spectrum analysis: a Hann-windowed FFT over the raw sample stream, folded into a fixed number
// of logarithmically spaced bands.
//
// Deliberately absent: noise gate, AGC, peak hold, magnitude smoothing. The captured stream is
// already the player's own signal (see audio.hpp), so anything normalised or smoothed on top of
// it would draw a spectrum the music does not have. The single mapping applied is a fixed
// dBFS window, which fixes where "silent" and "full height" are without looking at the material.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oms {

class Analyser {
 public:
  /** bands: number of output bars, 1..256. fftSize: window length, a power of two >= 64. */
  Analyser(int bands, int fftSize);

  /** Sample rate the samples arrive at, in Hz. Rescales the band edges, so call it again when
   *  PipeWire renegotiates the graph rate. Values below 1 are ignored. */
  void setRate(int rate);

  /** Appends mono samples, oldest first. Whatever cannot be folded into a window is dropped, so
   *  a caller that falls behind loses history rather than latency. */
  void feed(const float* samples, size_t count);

  /** bands() entries, each in 0..1, lowest frequency first. Valid after the first feed(). */
  const float* levels() const { return levels_.data(); }
  int bandCount() const { return static_cast<int>(levels_.size()); }
  int rate() const { return rate_; }

 private:
  /** Transforms the fftSize samples ending at absolute sample index `end` and folds them into
   *  the bands. */
  void analyse(uint64_t end);

  /** Bin ranges per band, from the log-spaced frequency edges. Rebuilt by setRate(). */
  void buildBands();

  /** In-place iterative radix-2 FFT over scratch_. Twiddles and the bit-reversal permutation are
   *  precomputed once; this runs a few times per rendered frame. */
  void transform();

  std::vector<float> levels_;   // bands(), 0..1
  std::vector<int> binLo_, binHi_;

  std::vector<float> hist_;     // fftSize most recent samples, a ring
  std::vector<float> window_;   // Hann, same length as the transform
  std::vector<float> re_, im_;  // transform scratch
  std::vector<int> rev_;        // bit-reversal permutation for transform()
  std::vector<float> twRe_, twIm_;

  uint64_t written_ = 0;    // samples ever written into hist_
  uint64_t analysed_ = 0;   // absolute index just past the last transformed window
  int rate_ = 0;
  int fftSize_ = 0;
  int hop_ = 0;
  int histMask_ = 0;
};

}  // namespace oms
