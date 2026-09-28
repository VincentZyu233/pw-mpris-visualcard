#pragma once
// Card rendering: hand-drawn with cairo + pango. All layout size/color constants are grouped at
// the top of card.cpp.
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <cstdint>
#include <string>

#include "art.hpp"
#include "types.hpp"

namespace oms {

class Card {
 public:
  explicit Card(Config cfg);
  ~Card();
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;

  /** Whether there is currently anything to display. */
  bool visible(const NowPlaying& np) const;

  /** Draw one frame. The cr canvas must be a cfg.size square; cover may be nullptr. */
  void render(cairo_t* cr, const NowPlaying& np, cairo_surface_t* cover, int64_t nowMs);

  int width() const { return cfg_.width; }
  int height() const { return cfg_.height; }
  const Config& config() const { return cfg_; }

 private:
  struct Metrics {
    double pad, radius, gap, metaGap;
    double coverD, ringGap, ringW;
    double titleSize, subSize, lyricSize, timeSize;
  };

  bool transparent() const { return !hasBg_; }

  /** Static layer: card background / cover drop shadow / cover circular-backdrop gradient /
   *  progress-ring track / all text. Text and cover do not overlap, so merging them into the same
   *  layer is entirely safe: one blit per frame. Reused as-is while the content (including the
   *  text key) is unchanged. */
  cairo_surface_t* staticLayer(const NowPlaying& np, int64_t pos, double k, double W,
                               double H, double cy, double yMeta, double textW);

  /** Cache key for the text layer. */
  std::string textKey(const NowPlaying& np, int64_t pos) const;

  void drawTexts(cairo_t* cr, const NowPlaying& np, int64_t pos, double yMeta,
                 double textW);

  /** Draw one text line (clamped to maxLines); with glow, stroke the shadow first, then fill. */
  void drawLine(cairo_t* cr, const std::string& s, double size, double alpha, double x,
                double width, double yTop, double boxH, int maxLines, bool bold);

  Config cfg_;
  Metrics m_;
  bool hasBg_ = true;
  double bgR_ = 0, bgG_ = 0, bgB_ = 0;

  SurfacePtr layer_;
  std::string layerKey_;

  /** Rotated cover layer (circular clip + edge antialiasing are baked in; one blit per frame).
   *  Uses a hand-written row-stepping rotation, about 30% faster than cairo's general transform
   *  path. */
  SurfacePtr coverLayer_;
  double coverAngle_ = 1e9;
  SurfacePtr artScaled_;      // cover scaled to coverD, with a 1px border
  std::string artScaledKey_;

  /** Rotate src (a square image with a 1px border) about its center by angle, writing into the
   *  circular region of dst. Within a row the source coordinates are equally spaced, so only the
   *  row's start value needs the two multiplications. */
  static void rotateInto(const uint32_t* src, int sw, int sh, uint32_t* dst, int dpitch,
                         int side, double radius, double angle);

  PangoLayout* layout_ = nullptr;
  PangoFontDescription* font_ = nullptr;
  double spinAngle_ = 0.0;
  int64_t lastRenderAt_ = 0;
};

}  // namespace oms
