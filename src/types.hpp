#pragma once
// pw-mpris-visualcard / native —— 共享数据类型
#include <cstdint>
#include <string>
#include <vector>

namespace oms {

/** LRC 一行：t = 毫秒 */
struct Lyric {
  int t = 0;
  std::string text;
};

struct Track {
  std::string id;
  std::string title;
  std::string artist;
  std::string album;
  std::string artUrl;
  int duration = 0;  // 毫秒
  std::vector<Lyric> lyrics;
};

/** 一次 MPRIS 采样结果。position 是 sampledAt 时刻的位置，靠 rate 外推。 */
struct NowPlaying {
  std::string status;  // "Playing" | "Paused" | "Stopped"
  std::string player;
  Track track;
  int64_t position = 0;   // 毫秒
  double rate = 1.0;
  int64_t sampledAt = 0;  // steady_clock 毫秒

  bool playing() const { return status == "Playing"; }
  bool paused() const { return status == "Paused"; }
};

/** 渲染与输出配置（全部由命令行参数填充） */
struct Config {
  int width = 360;             // 输出宽（px）
  int height = 360;            // 输出高（px）。等比缩放全部按 height 走
  int fps = 30;                // 推帧频率上限
  // 卡片底色：none / transparent = 完全透明（默认）；solid / dark = 不透明深色底；
  // 也可以直接给 #rrggbb。透明模式下会自动放大封面、提亮次级文字并加投影。
  std::string bg = "none";
  bool showProgress = true;    // 进度环
  bool showTime = false;       // 时间
  bool showAlbum = false;      // 专辑名
  int lyricLines = 0;          // 0 = 不显示歌词；N = 显示 N 行
  double spinSeconds = 24;     // 封面自转一圈的秒数，0 = 不转
  bool idleLast = false;       // true = 停止后保留最后一首
  std::string nodeName = "pw-mpris-visualcard";
  std::string nodeDescription = "Music Card";
};

}  // namespace oms
