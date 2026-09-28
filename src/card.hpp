#pragma once
// 卡片渲染：cairo + pango 手绘。所有版面尺寸/颜色常量集中在 card.cpp 顶部
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

  /** 当前是否有内容要显示 */
  bool visible(const NowPlaying& np) const;

  /** 画一帧。cr 的画布必须是 cfg.size 见方；cover 可为 nullptr。 */
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

  /** 静态层：卡片底 / 封面投影 / 封面圆底渐变 / 进度环底环 / 全部文字。
   *  文字和封面不重叠，所以并进同一层完全安全 —— 每帧只需一次 blit。
   *  内容（含文字 key）没变就直接复用。 */
  cairo_surface_t* staticLayer(const NowPlaying& np, int64_t pos, double k, double W,
                               double H, double cy, double yMeta, double textW);

  /** 文字层的缓存 key */
  std::string textKey(const NowPlaying& np, int64_t pos) const;

  void drawTexts(cairo_t* cr, const NowPlaying& np, int64_t pos, double yMeta,
                 double textW);

  /** 画一行（钳到 maxLines 行）文字；glow 时先描边做投影再填字 */
  void drawLine(cairo_t* cr, const std::string& s, double size, double alpha, double x,
                double width, double yTop, double boxH, int maxLines, bool bold);

  Config cfg_;
  Metrics m_;
  bool hasBg_ = true;
  double bgR_ = 0, bgG_ = 0, bgB_ = 0;

  SurfacePtr layer_;
  std::string layerKey_;

  /** 旋转后的封面图层（圆形裁剪 + 边缘抗锯齿都已烘焙进去，每帧只需一次 blit）。
   *  用自己写的逐行步进旋转，比 cairo 的通用变换路径快约 30%。 */
  SurfacePtr coverLayer_;
  double coverAngle_ = 1e9;
  SurfacePtr artScaled_;      // 按 coverD 缩放好的封面，带 1px 边框
  std::string artScaledKey_;

  /** 把 src（带 1px 边框的方形图）绕中心旋转 angle，写进 dst 的圆形区域。
   *  同一行内源坐标是等步长的，所以整行只有起始值需要两次乘法。 */
  static void rotateInto(const uint32_t* src, int sw, int sh, uint32_t* dst, int dpitch,
                         int side, double radius, double angle);

  PangoLayout* layout_ = nullptr;
  PangoFontDescription* font_ = nullptr;
  double spinAngle_ = 0.0;
  int64_t lastRenderAt_ = 0;
};

}  // namespace oms
