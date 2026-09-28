// pw-mpris-visualcard native - single process: MPRIS -> cairo rendering -> PipeWire video node
// Usage: see README.md (English, default) or README.zh-CN.md; --help prints a summary.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>

#include "art.hpp"
#include "card.hpp"
#include "mpris.hpp"
#include "pwvideo.hpp"
#include "types.hpp"

namespace {

using namespace oms;

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void usage() {
  std::printf(
      "pw-mpris-visualcard (native)\n"
      "\n"
      "  --size WxH      Output size. 540 = 540x540; 360x540 = portrait; default 360x360\n"
      "                  Layout scales by height; the width sets the side margins\n"
      "  --fps N         Frame-rate ceiling, default 30 (a consumer may go lower, never higher)\n"
      "  --bg MODE       Card background: none (default, fully transparent) | solid | #rrggbb\n"
      "  --progress 0|1  Progress ring, default 1\n"
      "  --time 0|1      Show time, default 0\n"
      "  --album 0|1     Show album, default 0\n"
      "  --lyrics N      Lyric lines, default 0 (off)\n"
      "  --spin SEC      Seconds per full cover rotation, 0 = no rotation, default 24\n"
      "  --idle last|hide  Keep the last track after playback stops, default hide\n"
      "  --node NAME     PipeWire node name, default pw-mpris-visualcard\n"
      "  --desc TEXT     Node description, default Music Card\n"
      "  --dump FILE     Render one sample to PNG and exit (for layout tuning)\n"
      "  --demo          Use fake data, no D-Bus connection (for layout tuning)\n"
      "  --verbose, -v   Log negotiation and frame pushes\n"
      "  --help\n");
}

bool parseArgs(int argc, char** argv, Config& cfg, std::string& dump, bool& demo,
               bool& verbose) {
  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error("missing argument value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage();
      return false;
    } else if (a == "--size") {
      const std::string v = next(i);
      const size_t x = v.find_first_of("xX*");
      if (x == std::string::npos) {
        cfg.width = cfg.height = std::max(64, std::stoi(v));
      } else {
        cfg.width = std::max(64, std::stoi(v.substr(0, x)));
        cfg.height = std::max(64, std::stoi(v.substr(x + 1)));
      }
    } else if (a == "--fps") {
      cfg.fps = std::max(1, std::stoi(next(i)));
    } else if (a == "--bg") {
      cfg.bg = next(i);
    } else if (a == "--progress") {
      cfg.showProgress = next(i) != "0";
    } else if (a == "--time") {
      cfg.showTime = next(i) != "0";
    } else if (a == "--album") {
      cfg.showAlbum = next(i) != "0";
    } else if (a == "--lyrics") {
      cfg.lyricLines = std::max(0, std::stoi(next(i)));
    } else if (a == "--spin") {
      cfg.spinSeconds = std::max(0.0, std::stod(next(i)));
    } else if (a == "--idle") {
      cfg.idleLast = next(i) == "last";
    } else if (a == "--node") {
      cfg.nodeName = next(i);
    } else if (a == "--desc") {
      cfg.nodeDescription = next(i);
    } else if (a == "--dump") {
      dump = next(i);
    } else if (a == "--demo") {
      demo = true;
    } else if (a == "--verbose" || a == "-v") {
      verbose = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return false;
    }
  }
  return true;
}

/* ---------------- Demo data (--demo) ---------------- */

struct DemoTrack {
  const char* title;
  const char* artist;
  const char* album;
  int duration;
};

const DemoTrack kDemo[] = {
    {"人造卫星", "三省, 星尘", "人造卫星", 192340},
    {"Night Drive", "Mirage Tape", "Neon Hours", 245000},
    {"沉溺于一场没有你的雨", "V.A.", "雨声收集者", 173000},
};

std::vector<Lyric> demoLyrics() {
  const char* lines[] = {"我是人造卫星", "绕着你旋转不停",
                         "穿过大气层的余温", "只为看你一眼"};
  std::vector<Lyric> out;
  for (int i = 0; i < 4; ++i) out.push_back(Lyric{2000 + i * 3000, lines[i]});
  return out;
}

NowPlaying demoState(int64_t now) {
  constexpr int64_t kStep = 10000;  // Switch tracks every 10 seconds, to inspect the layout quickly
  const int idx = static_cast<int>((now / kStep) % 3);
  const DemoTrack& d = kDemo[idx];
  NowPlaying np;
  np.status = "Playing";
  np.player = "demo";
  np.track.title = d.title;
  np.track.artist = d.artist;
  np.track.album = d.album;
  np.track.duration = d.duration;
  np.track.id = "/demo/" + std::to_string(idx);
  np.track.lyrics = demoLyrics();
  np.position = (now % kStep) * 8;  // Pretend to fast-forward
  np.rate = 1.0;
  np.sampledAt = now;
  return np;
}

/* ---------------- Application ---------------- */

class App {
 public:
  App(Config cfg, bool demo, bool verbose)
      : cfg_(std::move(cfg)), demo_(demo), verbose_(verbose), card_(cfg_) {}

  int run(const std::string& dumpPath) {
    if (!demo_) {
      mpris_.start();
      // Wait for the first sample, so the opening is not a blank frame
      for (int i = 0; i < 30 && !mpris_.healthy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!dumpPath.empty()) return dump(dumpPath);

    artThread_ = std::thread([this] { artLoop(); });

    pwvideo::Options opt;
    opt.width = cfg_.width;
    opt.height = cfg_.height;
    opt.fpsCap = cfg_.fps;
    opt.nodeName = cfg_.nodeName;
    opt.nodeDescription = cfg_.nodeDescription;
    opt.appName = "pw-mpris-visualcard";
    opt.verbose = verbose_;
    pwvideo::VideoNode video(opt, [this](uint8_t* dst, int stride, int w, int h) {
      renderInto(dst, stride, w, h);
    });
    video.start();
    std::printf(
        "pw-mpris-visualcard (native) started\n"
        "  PipeWire node: %s   [select it as a \"PipeWire Video\" source in OBS]\n"
        "  Size: %dx%d @ %d fps\n",
        cfg_.nodeName.c_str(), cfg_.width, cfg_.height, cfg_.fps);
    std::fflush(stdout);

    video.run();  // Blocks until SIGINT/SIGTERM
    stopping_.store(true);
    if (artThread_.joinable()) artThread_.join();
    return 0;
  }

 private:
  NowPlaying current() {
    return demo_ ? demoState(steadyMs()) : mpris_.snapshot();
  }

  /** Render one frame into dst (BGRA, premultiplied alpha) */
  void renderInto(uint8_t* dst, int dstStride, int w, int h) {
    const int FW = cfg_.width, FH = cfg_.height;
    std::lock_guard lock(frameMu_);
    if (!frame_ || frameW_ != FW || frameH_ != FH) {
      frame_.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, FW, FH),
                   CairoSurfaceDeleter{});
      frameW_ = FW;
      frameH_ = FH;
      frameCr_ = ContextPtr(cairo_create(frame_.get()));
    }
    SurfacePtr cover;
    {
      std::lock_guard lk(artMu_);
      cover = art_;
    }
    const int64_t now = steadyMs();
    const auto t0 = std::chrono::steady_clock::now();
    card_.render(frameCr_.get(), current(), cover.get(), now);
    const auto t1 = std::chrono::steady_clock::now();

    const auto t2 = std::chrono::steady_clock::now();
    const int srcStride = cairo_image_surface_get_stride(frame_.get());
    const uint8_t* src = cairo_image_surface_get_data(frame_.get());
    const int copyW = std::min(w, FW);
    const int copyH = std::min(h, FH);
    if (copyW == FW && copyH == FH && dstStride == srcStride) {
      std::memcpy(dst, src, static_cast<size_t>(srcStride) * FH);
    } else {
      for (int y = 0; y < copyH; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                    src + static_cast<size_t>(y) * srcStride,
                    static_cast<size_t>(copyW) * 4);
    }
    const auto t3 = std::chrono::steady_clock::now();
    statRender_ += std::chrono::duration<double, std::milli>(t1 - t0).count();
    statCopy_ += std::chrono::duration<double, std::milli>(t3 - t2).count();
    if (++statN_ == 120) {
      if (verbose_)
        std::fprintf(stderr, "[stat] render %.3f ms/frame   copy %.3f ms/frame\n",
                     statRender_ / statN_, statCopy_ / statN_);
      statRender_ = statCopy_ = 0;
      statN_ = 0;
    }
  }

  /** Background cover fetch: fetched only when the URL changes; a failed fetch is retried after
   *  30 seconds */
  void artLoop() {
    std::string want;
    int64_t retryAt = 0;
    while (!stopping_.load()) {
      const std::string url = current().track.artUrl;
      if (url != want) {
        want = url;
        retryAt = 0;
        std::lock_guard lk(artMu_);
        art_.reset();
      } else if (retryAt != 0 && steadyMs() >= retryAt) {
        retryAt = 0;
      } else if (retryAt != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }

      if (want.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }

      SurfacePtr s = loader_.get(want, cfg_.height);
      if (s) {
        std::lock_guard lk(artMu_);
        art_ = std::move(s);
      } else {
        retryAt = steadyMs() + 30000;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  int dump(const std::string& path) {
    const int FW = cfg_.width, FH = cfg_.height;
    SurfacePtr surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, FW, FH),
                    CairoSurfaceDeleter{});
    cairo_t* cr = cairo_create(surf.get());
    NowPlaying np = current();
    SurfacePtr cover;
    if (!np.track.artUrl.empty()) cover = loader_.get(np.track.artUrl, FH);
    card_.render(cr, np, cover.get(), steadyMs() + 1000);
    // Draw a checkerboard background so transparent areas can be confirmed by eye
    cairo_destroy(cr);
    const cairo_status_t st = cairo_surface_write_to_png(surf.get(), path.c_str());
    if (st != CAIRO_STATUS_SUCCESS) {
      std::fprintf(stderr, "PNG write failed: %s\n", cairo_status_to_string(st));
      return 1;
    }
    std::printf("Wrote %s (%dx%d)\n", path.c_str(), FW, FH);
    return 0;
  }

  Config cfg_;
  bool demo_;
  bool verbose_;
  Card card_;
  MprisClient mpris_;
  ArtLoader loader_;

  std::mutex artMu_;
  SurfacePtr art_;

  std::mutex frameMu_;
  SurfacePtr frame_;
  ContextPtr frameCr_;
  int frameW_ = 0, frameH_ = 0;

  std::atomic<bool> stopping_{false};
  std::thread artThread_;
  double statRender_ = 0, statCopy_ = 0;
  int statN_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string dumpPath;
  bool demo = false;
  bool verbose = false;
  try {
    if (!parseArgs(argc, argv, cfg, dumpPath, demo, verbose)) return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n", e.what());
    return 2;
  }

  try {
    App app(cfg, demo, verbose);
    return app.run(dumpPath);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }
}
