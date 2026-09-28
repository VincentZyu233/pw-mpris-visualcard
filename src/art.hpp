#pragma once
// 封面获取：libcurl 拉取 → gdk-pixbuf 解码 → 缩放到目标尺寸 → cairo ARGB32 表面
// 带小型 LRU 缓存，全程 RAII，不出现裸指针所有权。
#include <cairo/cairo.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace oms {

struct CairoSurfaceDeleter {
  void operator()(cairo_surface_t* s) const noexcept {
    if (s) cairo_surface_destroy(s);
  }
};
using SurfacePtr = std::shared_ptr<cairo_surface_t>;

struct CairoContextDeleter {
  void operator()(cairo_t* c) const noexcept {
    if (c) cairo_destroy(c);
  }
};
using ContextPtr = std::unique_ptr<cairo_t, CairoContextDeleter>;

class ArtLoader {
 public:
  ArtLoader();
  ~ArtLoader();
  ArtLoader(const ArtLoader&) = delete;
  ArtLoader& operator=(const ArtLoader&) = delete;

  /** 取封面：已缩放到 size×size 的 ARGB32（预乘 alpha）表面。失败返回 nullptr。 */
  SurfacePtr get(const std::string& url, int size);

  /** 清空缓存（换歌时按需调用，正常 LRU 会自己淘汰）。 */
  void clear();

  uint64_t fetched() const { return fetched_; }
  uint64_t failed() const { return failed_; }

 private:
  SurfacePtr fetch(const std::string& url, int size);

  struct Entry {
    std::string url;
    int size = 0;
    SurfacePtr surf;
  };

  std::mutex mu_;
  std::deque<Entry> cache_;  // 头部最新
  size_t cap_ = 3;
  uint64_t fetched_ = 0;
  uint64_t failed_ = 0;
  void* curl_ = nullptr;  // CURL*，只在锁内使用
};

}  // namespace oms
