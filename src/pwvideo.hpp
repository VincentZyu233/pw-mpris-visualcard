#pragma once
// PipeWire 视频输出：注册成 Stream/Output/Video 节点（obs-pwvideo 正是认这个 class）
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace oms {

/** 取一帧：往 dst 写 width×height 的 BGRA（预乘 alpha），stride 由调用方给定 */
using FrameProvider = std::function<void(uint8_t* dst, int dstStride, int width,
                                         int height)>;

class PwVideo {
 public:
  PwVideo(int width, int height, int fps, std::string nodeName,
          std::string nodeDescription, FrameProvider provider, bool verbose = false);
  ~PwVideo();
  PwVideo(const PwVideo&) = delete;
  PwVideo& operator=(const PwVideo&) = delete;

  /** 建立 PipeWire 连接并注册节点。失败抛 std::runtime_error。 */
  void start();
  /** 阻塞跑主循环，直到 stop() 被调用。 */
  void run();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oms
