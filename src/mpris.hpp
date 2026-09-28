#pragma once
// MPRIS client: one persistent D-Bus connection + a background sampling thread.
// The Node version forks one busctl per call; here the process count is constant at 0.
#include <memory>
#include <string>
#include <vector>

#include "types.hpp"

namespace oms {

/** Parse LRC text (MPRIS xesam:asText). Returns empty on fewer than two lines or no timestamps. */
std::vector<Lyric> parseLrc(const std::string& raw);

class MprisClient {
 public:
  MprisClient();
  ~MprisClient();
  MprisClient(const MprisClient&) = delete;
  MprisClient& operator=(const MprisClient&) = delete;

  /** Establish the connection and start the background sampling thread. Throws std::runtime_error on failure. */
  void start();
  void stop();

  /** Thread-safe snapshot. */
  NowPlaying snapshot() const;

  /** Whether the sampling thread is still alive (cleared to false when the connection drops). */
  bool healthy() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oms
