// pw-mpris-visualcard native —— 单进程：MPRIS → cairo 渲染 → PipeWire 视频节点
// 用法见 README.md（英文，默认）或 README.zh-CN.md；--help 有简表。
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
      "  --size WxH      输出尺寸。540 = 540x540；360x540 = 竖版；默认 360x360\n"
      "                  版式按高度缩放；宽度决定左右留白\n"
      "  --fps N         帧率上限，默认 30（消费者可以拉得更低，不会更高）\n"
      "  --bg MODE       卡片底色：none(默认，全透明) | solid | #rrggbb\n"
      "  --progress 0|1  进度环，默认 1\n"
      "  --time 0|1      显示时间，默认 0\n"
      "  --album 0|1     显示专辑，默认 0\n"
      "  --lyrics N      歌词行数，默认 0（关）\n"
      "  --spin SEC      封面自转一圈秒数，0 = 不转，默认 24\n"
      "  --idle last|hide  停止后是否保留最后一首，默认 hide\n"
      "  --node NAME     PipeWire 节点名，默认 pw-mpris-visualcard\n"
      "  --desc TEXT     节点描述，默认 Music Card\n"
      "  --dump FILE     采样一次渲染成 PNG 后退出（调版面用）\n"
      "  --demo          用假数据，不连 D-Bus（调版面用）\n"
      "  --verbose, -v   打印协商与推帧日志\n"
      "  --help\n");
}

bool parseArgs(int argc, char** argv, Config& cfg, std::string& dump, bool& demo,
               bool& verbose) {
  auto next = [&](int& i) -> std::string {
    if (i + 1 >= argc) throw std::runtime_error("缺少参数值");
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
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      usage();
      return false;
    }
  }
  return true;
}

/* ---------------- 演示数据（--demo） ---------------- */

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
  constexpr int64_t kStep = 10000;  // 每 10 秒换一首，方便快速看版式
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
  np.position = (now % kStep) * 8;  // 假装在快进
  np.rate = 1.0;
  np.sampledAt = now;
  return np;
}

/* ---------------- 应用 ---------------- */

class App {
 public:
  App(Config cfg, bool demo, bool verbose)
      : cfg_(std::move(cfg)), demo_(demo), verbose_(verbose), card_(cfg_) {}

  int run(const std::string& dumpPath) {
    if (!demo_) {
      mpris_.start();
      // 等第一次采样，避免开场空一帧
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
        "pw-mpris-visualcard (native) 已启动\n"
        "  PipeWire 节点: %s   [OBS 里用「PipeWire Video」源选它]\n"
        "  尺寸: %dx%d @ %d fps\n",
        cfg_.nodeName.c_str(), cfg_.width, cfg_.height, cfg_.fps);
    std::fflush(stdout);

    video.run();  // 阻塞到 SIGINT/SIGTERM
    stopping_.store(true);
    if (artThread_.joinable()) artThread_.join();
    return 0;
  }

 private:
  NowPlaying current() {
    return demo_ ? demoState(steadyMs()) : mpris_.snapshot();
  }

  /** 渲染一帧到 dst（BGRA，预乘 alpha） */
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
        std::fprintf(stderr, "[stat] 渲染 %.3f ms/帧   拷贝 %.3f ms/帧\n",
                     statRender_ / statN_, statCopy_ / statN_);
      statRender_ = statCopy_ = 0;
      statN_ = 0;
    }
  }

  /** 后台取封面：只在 URL 变化时抓，失败 30 秒后重试 */
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
    // 再画一层棋盘格背景，方便肉眼确认透明区域
    cairo_destroy(cr);
    const cairo_status_t st = cairo_surface_write_to_png(surf.get(), path.c_str());
    if (st != CAIRO_STATUS_SUCCESS) {
      std::fprintf(stderr, "写 PNG 失败: %s\n", cairo_status_to_string(st));
      return 1;
    }
    std::printf("已写出 %s (%dx%d)\n", path.c_str(), FW, FH);
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
    std::fprintf(stderr, "参数错误: %s\n", e.what());
    return 2;
  }

  try {
    App app(cfg, demo, verbose);
    return app.run(dumpPath);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "启动失败: %s\n", e.what());
    return 1;
  }
}
