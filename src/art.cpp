// 封面获取实现
#include "art.hpp"

#include <curl/curl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace oms {
namespace {

constexpr size_t kMaxImageBytes = 8u * 1024u * 1024u;

std::once_flag g_curlInit;

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* buf = static_cast<std::string*>(userdata);
  const size_t n = size * nmemb;
  if (buf->size() + n > kMaxImageBytes) return 0;  // 触发 curl 中止
  buf->append(ptr, n);
  return n;
}

struct PixbufUnref {
  void operator()(GdkPixbuf* p) const noexcept {
    if (p) g_object_unref(p);
  }
};
struct LoaderUnref {
  void operator()(GdkPixbufLoader* p) const noexcept {
    if (p) g_object_unref(p);
  }
};
using PixbufPtr = std::unique_ptr<GdkPixbuf, PixbufUnref>;
using LoaderPtr = std::unique_ptr<GdkPixbufLoader, LoaderUnref>;

struct FileCloser {
  void operator()(std::FILE* f) const noexcept {
    if (f) std::fclose(f);
  }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

/** 把 GdkPixbuf 转成 cairo ARGB32（小端即 BGRA，预乘 alpha） */
cairo_surface_t* pixbufToSurface(GdkPixbuf* pb) {
  const int w = gdk_pixbuf_get_width(pb);
  const int h = gdk_pixbuf_get_height(pb);
  const int nch = gdk_pixbuf_get_n_channels(pb);
  const int sstride = gdk_pixbuf_get_rowstride(pb);
  const guchar* src = gdk_pixbuf_get_pixels(pb);
  const bool alpha = gdk_pixbuf_get_has_alpha(pb) != 0;
  if (w <= 0 || h <= 0 || (nch != 3 && nch != 4)) return nullptr;

  cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return nullptr;
  }
  const int dstride = cairo_image_surface_get_stride(surf);
  uint8_t* dst = cairo_image_surface_get_data(surf);
  std::memset(dst, 0, static_cast<size_t>(dstride) * static_cast<size_t>(h));

  for (int y = 0; y < h; ++y) {
    const uint8_t* srow = src + static_cast<size_t>(y) * static_cast<size_t>(sstride);
    uint32_t* drow = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) *
                                                          static_cast<size_t>(dstride));
    for (int x = 0; x < w; ++x) {
      uint8_t r, g, b, a = 255;
      if (alpha) {
        r = srow[x * 4 + 0];
        g = srow[x * 4 + 1];
        b = srow[x * 4 + 2];
        a = srow[x * 4 + 3];
      } else {
        r = srow[x * 3 + 0];
        g = srow[x * 3 + 1];
        b = srow[x * 3 + 2];
      }
      const uint32_t pr = static_cast<uint32_t>(r) * a / 255u;
      const uint32_t pg = static_cast<uint32_t>(g) * a / 255u;
      const uint32_t pbit = static_cast<uint32_t>(b) * a / 255u;
      drow[x] = (static_cast<uint32_t>(a) << 24) | (pr << 16) | (pg << 8) | pbit;
    }
  }
  cairo_surface_mark_dirty(surf);
  return surf;
}

}  // namespace

ArtLoader::ArtLoader() {
  std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

ArtLoader::~ArtLoader() {
  std::lock_guard lock(mu_);
  if (curl_) {
    curl_easy_cleanup(static_cast<CURL*>(curl_));
    curl_ = nullptr;
  }
  cache_.clear();
}

void ArtLoader::clear() {
  std::lock_guard lock(mu_);
  cache_.clear();
}

SurfacePtr ArtLoader::get(const std::string& url, int size) {
  if (url.empty() || size <= 0) return nullptr;

  {
    std::lock_guard lock(mu_);
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->url == url && it->size == size) {
        Entry hit = *it;
        cache_.erase(it);
        cache_.push_front(std::move(hit));
        return cache_.front().surf;
      }
    }
  }

  // 网络和解码放在锁外，避免卡住其它线程；竞态最多重复抓一次，无害。
  SurfacePtr surf = fetch(url, size);
  if (!surf) {
    std::lock_guard lock(mu_);
    ++failed_;
    return nullptr;
  }

  std::lock_guard lock(mu_);
  ++fetched_;
  cache_.push_front(Entry{url, size, surf});
  while (cache_.size() > cap_) cache_.pop_back();
  return surf;
}

SurfacePtr ArtLoader::fetch(const std::string& url, int size) {
  std::string data;

  if (url.rfind("file://", 0) == 0) {
    // file:///path —— 无网络，直接读
    std::string path = url.substr(7);
    // 极简百分号解码（封面路径基本用不到，但保持一致）
    std::string decoded;
    decoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
      if (path[i] == '%' && i + 2 < path.size()) {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        const int hi = hex(path[i + 1]), lo = hex(path[i + 2]);
        if (hi >= 0 && lo >= 0) {
          decoded.push_back(static_cast<char>(hi * 16 + lo));
          i += 2;
          continue;
        }
      }
      decoded.push_back(path[i]);
    }
    FilePtr f(std::fopen(decoded.c_str(), "rb"));
    if (!f) return nullptr;
    char chunk[64 * 1024];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f.get())) > 0) {
      if (data.size() + n > kMaxImageBytes) return nullptr;
      data.append(chunk, n);
    }
  } else {
    CURL* curl = static_cast<CURL*>(curl_);
    if (!curl) {
      curl = curl_easy_init();
      if (!curl) return nullptr;
      curl_ = curl;
    }
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (pw-mpris-visualcard native)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) return nullptr;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code < 200 || code >= 300 || data.empty()) return nullptr;
  }

  if (data.empty()) return nullptr;

  LoaderPtr loader(gdk_pixbuf_loader_new());
  if (!loader) return nullptr;
  GError* err = nullptr;
  if (!gdk_pixbuf_loader_write(loader.get(),
                              reinterpret_cast<const guchar*>(data.data()),
                              static_cast<gsize>(data.size()), &err)) {
    if (err) g_error_free(err);
    return nullptr;
  }
  if (!gdk_pixbuf_loader_close(loader.get(), &err)) {
    if (err) g_error_free(err);
    return nullptr;
  }
  GdkPixbuf* raw = gdk_pixbuf_loader_get_pixbuf(loader.get());  // 归 loader 所有
  if (!raw) return nullptr;

  const int w = gdk_pixbuf_get_width(raw);
  const int h = gdk_pixbuf_get_height(raw);
  if (w <= 0 || h <= 0) return nullptr;

  // 先按“短边铺满”裁成正方形，再缩放——效果等同 CSS 的 object-fit: cover
  const int side = w < h ? w : h;
  const int offX = (w - side) / 2;
  const int offY = (h - side) / 2;
  PixbufPtr square(gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, side, side));
  if (!square) return nullptr;
  gdk_pixbuf_fill(square.get(), 0x00000000);
  gdk_pixbuf_copy_area(raw, offX, offY, side, side, square.get(), 0, 0);

  PixbufPtr scaled(side == size ? g_object_ref(square.get())
                                : gdk_pixbuf_scale_simple(square.get(), size, size,
                                                          GDK_INTERP_BILINEAR));
  if (!scaled) return nullptr;

  cairo_surface_t* surf = pixbufToSurface(scaled.get());
  if (!surf) return nullptr;
  return SurfacePtr(surf, CairoSurfaceDeleter{});
}

}  // namespace oms
