// 卡片渲染实现
//
// 性能要点：
//   1) cairo 的线性渐变填充非常慢（360×360 上一次约 0.6ms），所有每帧不变的东西
//      （卡片底、封面投影、封面圆底渐变、进度环底环）都烘焙进静态层，每帧只 blit。
//   2) 文字 shaping + 投影描边同样贵，所以和上面那些一起并进同一个静态层，
//      按内容做 key 缓存，只在换歌 / 歌词翻页 / 秒数变化时重做。
//
// 底色 --bg：
//   none（默认）—— 完全透明。封面放大到 92% 宽/纵向预算、次级文字提亮到 .9、
//                  文字和封面自带投影，否则叠在亮的游戏画面上会糊掉。
//   solid       —— 不透明深色底 #16171c。
//   #rrggbb     —— 指定底色。
#include "card.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace oms {
namespace {

constexpr double kPi = 3.14159265358979323846;

// 版面常量，全部以「源高 360px」为基准，实际按 height/360 等比缩放
constexpr double kBaseSize = 360.0;
constexpr double kPadPx = 12.0;
constexpr double kRadiusPx = 14.0 * 1.7;
constexpr double kGapPx = 11.0 * 0.9;
constexpr double kMetaGapPx = 2.0;
constexpr double kRingGapPx = 6.0;
constexpr double kRingWPx = 3.0;
constexpr double kLyricMarginTopPx = 3.0;

// 封面直径：有底板 min(62vh,84%) / min(48vh,72%)；无底板 min(68vh,92%) / min(54vh,82%)
constexpr double kCoverSolidNoLyric = 0.62;
constexpr double kCoverSolidLyric = 0.48;
constexpr double kCoverNoneNoLyric = 0.68;
constexpr double kCoverNoneLyric = 0.54;

// 颜色（与 :root 一致）
constexpr double kSolidR = 0x16 / 255.0, kSolidG = 0x17 / 255.0, kSolidB = 0x1c / 255.0;
constexpr double kFgDimSolid = 0.62;
constexpr double kFgDimNone = 0.90;   // 无底板时提亮，否则半透明白字会“化掉”
constexpr double kRingColor = 0.09;
constexpr double kTrackColor = 0.16;
constexpr double kAccentColor = 0.92;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** "none" / "transparent" / "0" → 无底色；"solid" / "dark" → 默认深色；"#rgb"/"#rrggbb" → 指定色 */
bool parseBg(const std::string& v, double& r, double& g, double& b) {
  std::string s;
  for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (s.empty() || s == "none" || s == "transparent" || s == "0") return false;
  if (s == "solid" || s == "dark" || s == "1") {
    r = kSolidR; g = kSolidG; b = kSolidB;
    return true;
  }
  if (s[0] == '#') s.erase(0, 1);
  if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
  if (s.size() != 6) { r = kSolidR; g = kSolidG; b = kSolidB; return true; }
  const int rr = hexVal(s[0]) * 16 + hexVal(s[1]);
  const int gg = hexVal(s[2]) * 16 + hexVal(s[3]);
  const int bb = hexVal(s[4]) * 16 + hexVal(s[5]);
  if (rr < 0 || gg < 0 || bb < 0) { r = kSolidR; g = kSolidG; b = kSolidB; return true; }
  r = rr / 255.0; g = gg / 255.0; b = bb / 255.0;
  return true;
}

double maxOf(double a, double b) { return a > b ? a : b; }

/** 双通道打包插值：R+B 一次算、G+A 一次算，权重和在 0..256 之间。
 *  只在权重和为 256 时安全。权重和写 65536 会让红色通道左移溢出被打飞。 */
inline uint32_t lerp2(uint32_t a, uint32_t b, uint32_t w) {
  const uint32_t iw = 256 - w;
  const uint32_t lo = (((a & 0x00FF00FF) * iw + (b & 0x00FF00FF) * w) >> 8) & 0x00FF00FF;
  const uint32_t hi =
      ((((a >> 8) & 0x00FF00FF) * iw + ((b >> 8) & 0x00FF00FF) * w) >> 8) & 0x00FF00FF;
  return lo | (hi << 8);
}

void roundRect(cairo_t* cr, double x, double y, double w, double h, double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
  cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
  cairo_arc(cr, x + r, y + r, r, kPi, 1.5 * kPi);
  cairo_close_path(cr);
}

std::string fmtTime(int64_t ms) {
  if (ms < 0) ms = 0;
  const int64_t s = ms / 1000;
  const int64_t h = s / 3600;
  const int64_t m = (s % 3600) / 60;
  const int64_t ss = s % 60;
  char buf[32];
  if (h > 0)
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m,
                  (long long)ss);
  else
    std::snprintf(buf, sizeof buf, "%lld:%02lld", (long long)m, (long long)ss);
  return buf;
}

/** 唱到第几句（二分），-1 表示还没到第一句 */
int activeLyric(const std::vector<Lyric>& L, int64_t ms) {
  int lo = 0, hi = static_cast<int>(L.size()) - 1, ans = -1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    if (L[static_cast<size_t>(mid)].t <= ms) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

}  // namespace

Card::Card(Config cfg) : cfg_(std::move(cfg)) {
  hasBg_ = parseBg(cfg_.bg, bgR_, bgG_, bgB_);

  const double W = cfg_.width, H = cfg_.height;
  const double k = H / kBaseSize;   // 所有尺寸按高度等比缩放

  m_.pad = hasBg_ ? kPadPx * k : 0.0;
  m_.radius = kRadiusPx * k;
  m_.gap = kGapPx * k;
  m_.metaGap = kMetaGapPx * k;
  m_.ringGap = kRingGapPx * k;
  m_.ringW = kRingWPx * k;
  m_.titleSize = maxOf(11.0 * k, 0.041 * H);
  m_.subSize = maxOf(9.0 * k, 0.029 * H);
  m_.lyricSize = maxOf(10.0 * k, 0.030 * H);
  m_.timeSize = maxOf(9.0 * k, 0.027 * H);

  // 封面直径：取「宽度允许」和「纵向剩余」的较小者。
  // 方形画布下纵向总是先到顶，于是左右必然空一截 —— 想收紧就把 --size 的宽改小。
  // 纵向按「有歌词」的最坏情况预留，避免切到有歌词的歌时封面突然缩放。
  const int nLy = cfg_.lyricLines;
  const double titleBox = 2.0 * 1.3 * m_.titleSize;
  const double subBox = 1.35 * m_.subSize;
  double metaH = titleBox + m_.metaGap + subBox;
  if (nLy > 0) {
    const double lyricBox = nLy * 1.35 * m_.lyricSize + (nLy - 1) * m_.metaGap +
                            kLyricMarginTopPx * k;
    metaH += m_.metaGap + lyricBox;
  }
  double restH = m_.gap + metaH;
  if (cfg_.showTime) restH += m_.gap + 1.35 * m_.timeSize;
  const double byWidth = 0.92 * W;
  const double byHeight = 0.96 * H - restH;
  m_.coverD = std::clamp(std::min(byWidth, byHeight), 0.20 * H, byWidth);

  cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t* cr = cairo_create(tmp);
  layout_ = pango_cairo_create_layout(cr);
  font_ = pango_font_description_new();
  pango_font_description_set_family(font_, "sans-serif");
  cairo_destroy(cr);
  cairo_surface_destroy(tmp);
}

Card::~Card() {
  if (font_) pango_font_description_free(font_);
  if (layout_) g_object_unref(layout_);
}

std::string Card::textKey(const NowPlaying& np, int64_t pos) const {
  const Track& t = np.track;
  const bool showTimes = cfg_.showTime && t.duration > 0;
  const int nLy = cfg_.lyricLines > 0
                      ? std::min<int>(cfg_.lyricLines, static_cast<int>(t.lyrics.size()))
                      : 0;
  const int act = activeLyric(t.lyrics, pos);
  std::string key;
  key.reserve(256);
  key += t.title;
  key.push_back('\x1f');
  key += t.artist;
  key.push_back('\x1f');
  key += t.album;
  key.push_back(cfg_.showAlbum ? '1' : '0');
  key.push_back('\x1f');
  key += showTimes ? fmtTime(pos) : std::string();
  key.push_back('\x1f');
  key += std::to_string(act);
  for (int i = 0; i < nLy; ++i) {
    const int idx = (act >= 0 ? act : 0) + i;
    if (idx >= static_cast<int>(t.lyrics.size())) break;
    key.push_back('\x1e');
    key += t.lyrics[static_cast<size_t>(idx)].text;
  }
  return key;
}

cairo_surface_t* Card::staticLayer(const NowPlaying& np, int64_t pos, double k, double W,
                                   double H, double cy, double yMeta, double textW) {
  const int SZ = static_cast<int>(H);
  std::string key = std::to_string(cfg_.width) + "x" + std::to_string(SZ) + "#" +
                    std::to_string(static_cast<int>(cy * 2)) + "#" + textKey(np, pos);
  if (layer_ && key == layerKey_) return layer_.get();

  cairo_surface_t* surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cfg_.width, SZ);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return nullptr;
  }
  cairo_t* cr = cairo_create(surf);
  const double cx = W / 2.0;
  const double coverR = m_.coverD / 2.0;

  if (hasBg_) {
    // 外阴影 0 6px 22px rgba(0,0,0,.32)：几层递减描边近似（只在卡片圆角外可见）
    const double dy = 6.0 * k, blur = 22.0 * k;
    static const double widths[] = {1.0, 0.72, 0.45, 0.22};
    static const double alphas[] = {0.02, 0.05, 0.08, 0.11};
    for (int i = 0; i < 4; ++i) {
      cairo_set_line_width(cr, blur * widths[i]);
      cairo_set_source_rgba(cr, 0, 0, 0, alphas[i]);
      roundRect(cr, 0, dy, W, H, m_.radius + blur * widths[i] / 2.0);
      cairo_stroke(cr);
    }
    roundRect(cr, 0, 0, W, H, m_.radius);
    cairo_set_source_rgb(cr, bgR_, bgG_, bgB_);
    cairo_fill(cr);
    roundRect(cr, 0.5, 0.5, W - 1, H - 1, m_.radius - 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1, 1, 1, kRingColor);
    cairo_stroke(cr);
  } else {
    // 无底板：封面得自带柔和投影，否则叠在亮画面上边缘会化掉。
    // 用几圈向外扩散、逐层变淡的描边近似 drop-shadow。
    static const double spread[] = {1.5, 3.4, 5.6};
    static const double alpha[] = {0.22, 0.14, 0.07};
    for (int i = 0; i < 3; ++i) {
      cairo_set_line_width(cr, 3.0 * k);
      cairo_set_source_rgba(cr, 0, 0, 0, alpha[i]);
      cairo_arc(cr, cx, cy + 1.5 * k, coverR + spread[i] * k, 0, 2 * kPi);
      cairo_stroke(cr);
    }
  }

  // 封面圆底：CSS linear-gradient(150deg, --track, transparent)。这是全画面最贵的一笔
  {
    const double ang = 150.0 * kPi / 180.0;
    const double dx = std::sin(ang), dy = -std::cos(ang);
    const double len = m_.coverD * (std::abs(dx) + std::abs(dy));
    cairo_pattern_t* g = cairo_pattern_create_linear(cx - dx * len / 2, cy - dy * len / 2,
                                                     cx + dx * len / 2,
                                                     cy + dy * len / 2);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, kTrackColor);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0.0);
    cairo_arc(cr, cx, cy, coverR, 0, 2 * kPi);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
  }
  // art-wrap 的 1px 内描边
  cairo_arc(cr, cx, cy, coverR - 0.5, 0, 2 * kPi);
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgba(cr, 1, 1, 1, kRingColor);
  cairo_stroke(cr);

  // 进度环底环也是静态的（描边很贵，别每帧画）
  if (cfg_.showProgress) {
    const double ringR = coverR + m_.ringGap - m_.ringW / 2.0;
    cairo_arc(cr, cx, cy, ringR, 0, 2 * kPi);
    cairo_set_line_width(cr, m_.ringW);
    cairo_set_source_rgba(cr, 1, 1, 1, kTrackColor);
    cairo_stroke(cr);
  }

  // 文字并进同一层（含无底板时的投影描边）
  drawTexts(cr, np, pos, yMeta, textW);

  cairo_destroy(cr);
  layer_ = SurfacePtr(surf, CairoSurfaceDeleter{});
  layerKey_ = std::move(key);
  return layer_.get();
}

bool Card::visible(const NowPlaying& np) const {
  if (np.track.title.empty() && np.track.artist.empty()) return false;
  return np.playing() || np.paused() || cfg_.idleLast;
}

void Card::drawLine(cairo_t* cr, const std::string& s, double size, double alpha,
                    double x, double width, double yTop, double boxH, int maxLines,
                    bool bold) {
  if (s.empty() || width <= 0 || boxH <= 0) return;

  pango_cairo_update_layout(cr, layout_);
  pango_font_description_set_absolute_size(font_, size * PANGO_SCALE);
  pango_font_description_set_weight(font_, bold ? PANGO_WEIGHT_SEMIBOLD
                                                : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description(layout_, font_);
  pango_layout_set_text(layout_, s.c_str(), -1);
  pango_layout_set_width(layout_, static_cast<int>(width * PANGO_SCALE));
  pango_layout_set_alignment(layout_, PANGO_ALIGN_CENTER);
  pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_ellipsize(layout_, PANGO_ELLIPSIZE_END);
  pango_layout_set_height(layout_, -maxLines);  // 负数 = 最多 N 行

  int pw = 0, ph = 0;
  pango_layout_get_pixel_size(layout_, &pw, &ph);
  const double ty = yTop + (boxH - ph) / 2.0;

  // 无底板时先描边做出投影：两层由外到内、逐层加浓，近似 CSS 的三层 text-shadow
  if (!hasBg_) {
    const double k = cfg_.height / kBaseSize;
    cairo_save(cr);
    cairo_translate(cr, 0, 1.5 * k);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    static const double glowW[] = {7.0, 3.5};
    static const double glowA[] = {0.35, 0.60};
    for (int i = 0; i < 2; ++i) {
      cairo_move_to(cr, x, ty);
      pango_cairo_layout_path(cr, layout_);
      cairo_set_source_rgba(cr, 0, 0, 0, glowA[i]);
      cairo_set_line_width(cr, glowW[i] * k);
      cairo_stroke(cr);
    }
    cairo_restore(cr);
  }

  cairo_save(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, alpha);
  cairo_move_to(cr, x, ty);
  pango_cairo_show_layout(cr, layout_);
  cairo_restore(cr);
}

void Card::rotateInto(const uint32_t* src, int sw, int sh, uint32_t* dst, int dpitch,
                      int side, double radius, double angle) {
  const double cx = side / 2.0, cy = side / 2.0, R = radius;
  const double scx = sw / 2.0, scy = sh / 2.0;
  const double inner = (R - 0.5) * (R - 0.5);

  // 源坐标和旋转都用 16.16 定点：省掉每像素的 double 与 floor。
  // 实测这一版比双精度逐通道版快 43%（0.754 → 0.431 ms @327px）。
  const int32_t ca = static_cast<int32_t>(std::llround(std::cos(angle) * 65536.0));
  const int32_t sa = static_cast<int32_t>(std::llround(std::sin(angle) * 65536.0));
  const int32_t SCX = static_cast<int32_t>(std::llround(scx * 65536.0));
  const int32_t SCY = static_cast<int32_t>(std::llround(scy * 65536.0));

  for (int y = 0; y < side; ++y) {
    const double dy = (y + 0.5) - cy;
    if (std::abs(dy) >= R) continue;
    const double half = std::sqrt(R * R - dy * dy);
    int xa = static_cast<int>(std::ceil(cx - half - 0.5));
    int xb = static_cast<int>(std::floor(cx + half - 0.5));
    if (xa < 0) xa = 0;
    if (xb >= side) xb = side - 1;

    uint32_t* drow = dst + static_cast<size_t>(y) * dpitch;
    const int32_t DY = static_cast<int32_t>(std::llround(dy * 65536.0));
    const int32_t DX0 = static_cast<int32_t>(std::llround(((xa + 0.5) - cx) * 65536.0));
    int32_t sx = static_cast<int32_t>((static_cast<int64_t>(ca) * DX0 +
                                       static_cast<int64_t>(sa) * DY) >> 16) + SCX;
    int32_t sy = static_cast<int32_t>((static_cast<int64_t>(-sa) * DX0 +
                                       static_cast<int64_t>(ca) * DY) >> 16) + SCY;

    for (int x = xa; x <= xb; ++x, sx += ca, sy += -sa) {
      const double px = (x + 0.5) - cx;
      const double d2 = px * px + dy * dy;
      double cov = 1.0;
      if (d2 > inner) {  // 只在 1px 圆环上算 sqrt
        const double d = std::sqrt(d2);
        cov = R + 0.5 - d;
        if (cov <= 0.0) continue;
        if (cov > 1.0) cov = 1.0;
      }
      const int32_t fx = sx - 32768;  // 扣除 0.5 像素偏移
      const int32_t fy = sy - 32768;
      const int ix = fx >> 16;        // 算术右移 = floor
      const int iy = fy >> 16;
      const uint32_t tx = static_cast<uint32_t>((fx >> 8) & 0xFF);
      const uint32_t ty = static_cast<uint32_t>((fy >> 8) & 0xFF);
      // 源带 1px 边框，正常落在 [-1, sw-2]；这个判断只是兜底
      if (ix < -1 || iy < -1) continue;
      const uint32_t* r0 = src + static_cast<size_t>(iy) * sw + ix;
      uint32_t out = lerp2(lerp2(r0[0], r0[1], tx), lerp2(r0[sw], r0[sw + 1], tx), ty);

      if (cov < 1.0) {  // 圆边羽化；预乘 alpha，四通道一起缩
        const uint32_t c = static_cast<uint32_t>(cov * 256.0);
        const uint32_t lo = (((out & 0x00FF00FF) * c) >> 8) & 0x00FF00FF;
        const uint32_t hi = ((((out >> 8) & 0x00FF00FF) * c) >> 8) & 0x00FF00FF;
        out = lo | (hi << 8);
      }
      drow[x] = out;
    }
  }
}

void Card::drawTexts(cairo_t* cr, const NowPlaying& np, int64_t pos, double yMeta,
                     double textW) {
  const Track& t = np.track;
  const bool none = !hasBg_;
  const double fgDim = none ? kFgDimNone : kFgDimSolid;
  const double lyricIdle = fgDim * (none ? 0.85 : 0.6);

  const double titleBox = 2.0 * 1.3 * m_.titleSize;
  const double subBox = 1.35 * m_.subSize;
  const int nLy = cfg_.lyricLines > 0
                      ? std::min<int>(cfg_.lyricLines, static_cast<int>(t.lyrics.size()))
                      : 0;

  double y = yMeta;
  drawLine(cr, t.title, m_.titleSize, 1.0, m_.pad, textW, y, titleBox, 2, true);
  y += titleBox + m_.metaGap;

  std::string sub = t.artist;
  if (cfg_.showAlbum && !t.album.empty()) {
    if (!sub.empty()) sub += " \u00b7 ";
    sub += t.album;
  }
  drawLine(cr, sub, m_.subSize, fgDim, m_.pad, textW, y, subBox, 1, false);
  y += subBox + m_.metaGap;

  if (nLy > 0) {
    y += kLyricMarginTopPx * (cfg_.height / kBaseSize);
    const int act = activeLyric(t.lyrics, pos);
    const int start = act >= 0 ? act : 0;
    for (int i = 0; i < nLy; ++i) {
      const int idx = start + i;
      if (idx >= static_cast<int>(t.lyrics.size())) break;
      const bool isActive = (idx == act);
      drawLine(cr, t.lyrics[static_cast<size_t>(idx)].text, m_.lyricSize,
               isActive ? 1.0 : lyricIdle, m_.pad, textW, y, 1.35 * m_.lyricSize, 1,
               isActive);
      y += 1.35 * m_.lyricSize + m_.metaGap;
    }
    y -= m_.metaGap;
  }

  if (cfg_.showTime && t.duration > 0) {
    y += m_.gap;
    drawLine(cr, fmtTime(pos) + " / " + fmtTime(t.duration), m_.timeSize, fgDim, m_.pad,
             textW, y, 1.35 * m_.timeSize, 1, false);
  }
}

void Card::render(cairo_t* cr, const NowPlaying& np, cairo_surface_t* cover,
                  int64_t nowMs) {
  const double W = cfg_.width;
  const double H = cfg_.height;
  const double k = H / kBaseSize;
  const Track& t = np.track;

  // 封面自转：暂停时冻结
  if (lastRenderAt_ != 0 && np.playing() && cfg_.spinSeconds > 0) {
    const double dt = static_cast<double>(nowMs - lastRenderAt_) / 1000.0;
    if (dt > 0 && dt < 1.0) spinAngle_ += dt / cfg_.spinSeconds * 2 * kPi;
  }
  lastRenderAt_ = nowMs;

  if (!visible(np)) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return;
  }

  /* ---------------- 先算布局 ---------------- */
  const double coverR = m_.coverD / 2.0;
  const double titleBox = 2.0 * 1.3 * m_.titleSize;
  const double subBox = 1.35 * m_.subSize;
  const int nLy = cfg_.lyricLines > 0
                      ? std::min<int>(cfg_.lyricLines, static_cast<int>(t.lyrics.size()))
                      : 0;
  const double lyricBox =
      nLy > 0
          ? nLy * 1.35 * m_.lyricSize + (nLy - 1) * m_.metaGap + kLyricMarginTopPx * k
          : 0.0;
  const bool showTimes = cfg_.showTime && t.duration > 0;
  const double timeBox = showTimes ? 1.35 * m_.timeSize : 0.0;

  double metaH = titleBox + m_.metaGap + subBox;
  if (nLy > 0) metaH += m_.metaGap + lyricBox;
  const double total = m_.coverD + m_.gap + metaH + (showTimes ? m_.gap + timeBox : 0.0);

  double y = (H - total) / 2.0;
  const double cx = W / 2.0;
  const double cy = y + coverR;
  const double yMeta = y + m_.coverD + m_.gap;
  const double textW = W - 2 * m_.pad;

  int64_t pos = np.position;
  if (np.playing()) pos += static_cast<int64_t>((nowMs - np.sampledAt) * np.rate);
  const int64_t textPos = showTimes ? std::clamp<int64_t>(pos, 0, t.duration) : pos;

  /* ---------------- 静态层（含文字）：一步覆盖整帧 ---------------- */
  {
    cairo_surface_t* bgs = staticLayer(np, textPos, k, W, H, cy, yMeta, textW);
    if (bgs) {
      cairo_save(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_surface(cr, bgs, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }

  const bool dim = !np.playing() && !np.paused();
  if (dim) cairo_push_group(cr);

  /* ---------------- 封面 ---------------- */
  if (cover) {
    const int sw = cairo_image_surface_get_width(cover);
    const int sh = cairo_image_surface_get_height(cover);
    if (sw > 0 && sh > 0) {
      const int side = static_cast<int>(m_.coverD) + 2;

      // 1) 按 coverD 缩放一次（每首歌一次），带 1px 边框
      char ak[96];
      std::snprintf(ak, sizeof ak, "%p#%d", static_cast<const void*>(cover), side);
      if (!artScaled_ || artScaledKey_ != ak) {
        cairo_surface_t* ss = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
        if (cairo_surface_status(ss) == CAIRO_STATUS_SUCCESS) {
          cairo_t* sc = cairo_create(ss);
          cairo_translate(sc, 1, 1);
          cairo_scale(sc, m_.coverD / sw, m_.coverD / sh);
          cairo_set_source_surface(sc, cover, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(sc), CAIRO_FILTER_BILINEAR);
          cairo_paint(sc);
          cairo_destroy(sc);
          artScaled_ = SurfacePtr(ss, CairoSurfaceDeleter{});
          artScaledKey_ = ak;
          coverLayer_.reset();
          coverAngle_ = 1e9;
        } else {
          cairo_surface_destroy(ss);
        }
      }

      // 2) 每帧真旋转。按角度分档能省 0.4ms，但 24 秒一圈时只有 7.5Hz 更新，
      //    肉眼就是明显的卡顿 —— 所以这里不平滑就不省。
      if (artScaled_) {
        if (!coverLayer_ ||
            cairo_image_surface_get_width(coverLayer_.get()) != side ||
            spinAngle_ != coverAngle_) {
          if (!coverLayer_ ||
              cairo_image_surface_get_width(coverLayer_.get()) != side) {
            cairo_surface_t* cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
            if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
              cairo_surface_destroy(cs);
              cs = nullptr;
            }
            coverLayer_ = cs ? SurfacePtr(cs, CairoSurfaceDeleter{}) : nullptr;
          }
          if (coverLayer_) {
            cairo_surface_flush(coverLayer_.get());
            rotateInto(
                reinterpret_cast<const uint32_t*>(
                    cairo_image_surface_get_data(artScaled_.get())),
                side, side,
                reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(coverLayer_.get())),
                cairo_image_surface_get_stride(coverLayer_.get()) / 4, side, m_.coverD / 2.0,
                spinAngle_);
            cairo_surface_mark_dirty(coverLayer_.get());
            coverAngle_ = spinAngle_;
          }
        }
        if (coverLayer_) {
          cairo_set_source_surface(cr, coverLayer_.get(), cx - coverR - 1, cy - coverR - 1);
          cairo_paint(cr);
        }
      }
    }
  } else {
    // .card.no-art 的 “♪” 占位
    drawLine(cr, "\u266a", maxOf(18.0 * k, 0.14 * H), kFgDimSolid, cx - coverR,
             m_.coverD, cy - coverR, m_.coverD, 1, false);
  }

  /* ---------------- 进度弧 ---------------- */
  if (cfg_.showProgress && t.duration > 0) {
    const double ringR = coverR + m_.ringGap - m_.ringW / 2.0;
    const int64_t clamped = std::clamp<int64_t>(pos, 0, t.duration);
    const double p = static_cast<double>(clamped) / static_cast<double>(t.duration);
    if (p > 0) {
      cairo_set_line_width(cr, m_.ringW);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
      cairo_arc(cr, cx, cy, ringR, -kPi / 2, -kPi / 2 + p * 2 * kPi);
      cairo_set_source_rgba(cr, 1, 1, 1, kAccentColor);
      cairo_stroke(cr);
    }
  }

  if (dim) {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.5);
  }
}

}  // namespace oms
