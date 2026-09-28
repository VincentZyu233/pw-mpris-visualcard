#pragma once
// MPRIS 客户端：一条常驻 D-Bus 连接 + 后台采样线程。
// 对比 Node 版每次调用 fork 一个 busctl —— 这里进程数恒定为 0。
#include <memory>
#include <string>
#include <vector>

#include "types.hpp"

namespace oms {

/** 解析 LRC 文本（MPRIS 的 xesam:asText）。不足两行或没有时间戳返回空。 */
std::vector<Lyric> parseLrc(const std::string& raw);

class MprisClient {
 public:
  MprisClient();
  ~MprisClient();
  MprisClient(const MprisClient&) = delete;
  MprisClient& operator=(const MprisClient&) = delete;

  /** 建立连接并启动后台采样线程。失败抛 std::runtime_error。 */
  void start();
  void stop();

  /** 线程安全快照。 */
  NowPlaying snapshot() const;

  /** 采样线程是否还活着（连接断了会置 false）。 */
  bool healthy() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oms
