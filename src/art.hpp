#pragma once
// Cover fetching: libcurl download -> gdk-pixbuf decode -> scale to target size -> cairo ARGB32
// surface. Small LRU cache; RAII throughout, no raw-pointer ownership.
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

  /** Fetch a cover: an ARGB32 (premultiplied alpha) surface already scaled to size x size.
   *  Returns nullptr on failure. */
  SurfacePtr get(const std::string& url, int size);

  /** Clear the cache (call on demand when the track changes; under normal operation the LRU
   *  evicts on its own). */
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
  std::deque<Entry> cache_;  // newest at the front
  size_t cap_ = 3;
  uint64_t fetched_ = 0;
  uint64_t failed_ = 0;
  void* curl_ = nullptr;  // CURL*, used only while holding the lock
};

}  // namespace oms
