# pw-mpris-visualcard

[English](README.md) · [简体中文](README.zh-CN.md)

把本机正在播放的音乐渲染成一张卡片，以 **PipeWire 视频节点**的形式直接输出给 OBS。

不经过浏览器、CEF 或 HTTP：单进程、零子进程。没有消费者连接时完全不渲染。

## 工作方式

| 阶段 | 输入 | 输出 | 实现 |
| --- | --- | --- | --- |
| 状态采样 | 播放器的 MPRIS 接口（D-Bus） | `NowPlaying` 快照：标题、歌手、专辑、进度、歌词、封面 URL | `mpris`，常驻 D-Bus 连接，不 fork 子进程 |
| 素材获取 | 封面 URL | cairo 表面（LRU 缓存，最多 3 张） | `art`，后台线程 + libcurl + gdk-pixbuf |
| 版面渲染 | 快照 + 封面表面 | BGRA 帧（预乘 alpha） | `card`，cairo + pango |
| 视频输出 | BGRA 帧 | `Stream/Output/Video` 节点 | `pwvideo`，libpipewire |

各阶段都在同一进程内通过内存传递数据，进程私有内存 6–25 MB。

## 特性

- 播放信息来自 D-Bus / MPRIS（musicfox、Spotify、VLC、mpv、Rhythmbox 等），一条常驻连接、零子进程；轮询式实现每秒需要 fork 若干次 `busctl`。
- 默认完全透明背景，画面上只有封面圆盘与文字；两者自带投影，叠在明亮内容上依然可读。
- 同步歌词读取 MPRIS 的 `xesam:asText`（LRC）。当前句固定在首行槽位并高亮，其下跟随后续行，整块不跳动。
- 封面自转，暂停时冻结。
- 进度环、时间、专辑名可分别开关。
- 尺寸任意（`WxH`），版式按高度等比缩放。
- 帧率上限可调；消费者可以协商到更低，但不会超过上限。
- 调版面无需打开 OBS：`--dump` 直接输出 PNG。

## 效果

下列 PNG 由实机播放时 `--dump` 输出。背景本身是透明的，此处按原样展示。

![卡片效果](docs/card-460x690.png)

`--size 460x690 --lyrics 4 --time 1 --album 1`：圆封面（自转）+ 标题 + 歌手 · 专辑 + 当前句高亮 + 后续歌词压暗 + 进度环 + `0:38 / 2:47`。

默认参数（`360x360`，不开歌词 / 时间 / 专辑）即最小形态：

![默认形态](docs/card-360x360.png)

## 快速开始

### 1. 安装

两条路，装出来的二进制同名（`pw-mpris-visualcard-native`）。

**从 AUR 安装**——由包管理器构建，不必自己维护一份仓库：

```bash
paru -S pw-mpris-visualcard-git     # 或 yay -S pw-mpris-visualcard-git
```

`pw-mpris-visualcard-git` 提供并冲突于 `pw-mpris-visualcard`，支持 `x86_64` 与 `aarch64`。装出来的是 `/usr/bin/pw-mpris-visualcard-native`、`/usr/share/doc/pw-mpris-visualcard/` 下的文档，以及**已经渲染好的** systemd 用户 unit——所以第 6 步的 `make install-service` 不用执行（也不能执行，原因见该节）。构建走 `PORTABLE=1`（见第 3 步）。`obs-pwvideo` 与 CJK 字体（`noto-fonts-cjk`）列为可选依赖。

作为 `-git` 包，它跟随最新提交，重新构建才会拉到新版本。

**从源码安装**——即下面两步。

### 2. 依赖

依赖全部来自发行版仓库，不涉及任何语言包管理器。（仅源码构建需要；AUR 包已把这些库作为硬依赖拉入。）

ArchLinux：

```bash
# Arch
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire sdbus-cpp curl
```

OBS 侧需要一个能选择任意 PipeWire 节点的插件。OBS 自带的 `linux-pipewire` 走 xdg-desktop-portal，只能捕获屏幕与窗口，抓不到本节点；需要配合插件 [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) 使用。

### 3. 构建

```bash
make             # → ./pw-mpris-visualcard-native
make PORTABLE=1  # 同上，但不加 -march=native（换机器运行或分发时使用）
```

默认使用 `-O3 -march=native -funroll-loops`：封面旋转的热循环收益明显，整帧实测降低 22%。代价是二进制绑定本机指令集，因此保留 `PORTABLE=1`。

### 4. 运行

```bash
./pw-mpris-visualcard-native
```

用 AUR 包安装的话二进制已在 `PATH` 中，去掉 `./`。

终端会打印节点名（默认 `pw-mpris-visualcard`）。

### 5. 在 OBS 中接入

1. 来源 **+** → **PipeWire Video**（由 `obs-pwvideo` 提供）。
2. 在「Source」下拉框中选择 **Music Card**。下拉框里显示的是节点描述（`--desc`，默认 `Music Card`），选中后实际连接的节点名是 `--node`（默认 `pw-mpris-visualcard`）；两者都可以改，别只看显示名。
3. **宽高设置为与 `--size` 一致**（默认 `360 × 360`），保证 1:1 不被缩放。

透明通道原样透传，直接叠加到场景中即可。

> 下拉框中看不到节点？该插件只在**打开对话框的瞬间**枚举一次节点，不会自动刷新：先确认进程在运行，再重开一次属性窗口。此外节点必须同时满足 `media.type=Video`、`media.class=Stream/Output/Video`、`media.role=Production` 三项，缺一项就完全不显示。细节见 [docs/internals.md](docs/internals.md)。

### 6. 开机自启（可选）

**从 AUR 安装**的话 unit 已经渲染好放在 `/usr/lib/systemd/user/pw-mpris-visualcard.service`，参数是 `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`，启用即可：

```bash
systemctl --user enable --now pw-mpris-visualcard
systemctl --user edit pw-mpris-visualcard    # 改参数：覆盖 ExecStart=
```

这种情况下**不要**再跑 `make install-service`：它写出的 unit 落在 `~/.config/systemd/user/`，会**遮蔽**包里的那一个，且指向某个 checkout。卸载则是 `pacman -Rns pw-mpris-visualcard-git`。

**从源码安装**的话，仓库里的 `pw-mpris-visualcard.service` 是**模板**，含 `@REPO@`（仓库绝对路径）和 `@ARGS@`（启动参数）两个占位符，**不能直接复制使用**——systemd 在加载阶段就会判定 unit 配置致命错误（`WorkingDirectory= path is not absolute: @REPO@`），unit 不会被启动。用 `make` 渲染安装：

```bash
make install-service                            # 渲染 unit 到 ~/.config/systemd/user/
systemctl --user enable --now pw-mpris-visualcard    # 启用并立即启动
```

默认启动参数是 `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`，要改就覆盖 `SERVICE_ARGS`：

```bash
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
systemctl --user restart pw-mpris-visualcard         # 改过参数后重启才生效
```

`make install-service` 只写 unit 并 `daemon-reload`，不会替你 enable 或启动，机器原有状态不受影响。卸载：

```bash
make uninstall-service
```

> 手动安装同样可行，但必须先把 unit 里两个占位符替换成实际值。另外服务设了 `PrivateTmp=yes`，`--dump` 的输出会落在服务私有的 `/tmp` 里取不出来——调版面请直接在终端跑 `--dump`，别写进 unit。

## 命令行参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--size WxH` | `360x360` | 输出尺寸；只写一个数字表示正方形。**版式全部按高度等比缩放，宽度只决定左右留白**——圆形封面受纵向预算约束，方画布横向必然富余较多 |
| `--fps N` | `30` | **帧率上限**。向 PipeWire 声明的是 `[N/4, N]` 区间，消费者可协商到更低但不会超过 N；实际推帧按协商结果执行，下限 5fps |
| `--bg MODE` | `none` | `none` 完全透明；`solid` 不透明深色底；也可给 `#rrggbb` |
| `--progress 0\|1` | `1` | 进度环 |
| `--time 0\|1` | `0` | 显示 `1:23 / 3:12` |
| `--album 0\|1` | `0` | 歌手后追加专辑名 |
| `--lyrics N` | `0` | 歌词行数，`0` 为关闭 |
| `--spin SEC` | `24` | 封面自转一圈的秒数，`0` 为不转 |
| `--idle last\|hide` | `hide` | 停止播放后是否保留最后一首 |
| `--node NAME` | `pw-mpris-visualcard` | PipeWire 节点名，也是 OBS 下拉项背后的取值。同名节点会各自带 `(id)` 后缀区分 |
| `--desc TEXT` | `Music Card` | 节点描述。**OBS 下拉框里显示的就是它**，不是 `--node` |
| `--verbose`, `-v` | | 打印协商、推帧与每帧耗时 |
| `--dump FILE` | | 采样一次渲染为 PNG 后退出 |
| `--demo` | | 使用假数据，不连接 D-Bus |
| `--help`, `-h` | | 打印参数简表 |

### 调版面无需打开 OBS

```bash
make dump                                              # 假数据输出一张 PNG
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1  # 使用真实播放数据
./pw-mpris-visualcard-native --demo --dump /tmp/card.png --lyrics 4 --time 1 --album 1
```

## 性能

| 指标 | 数值 |
| --- | --- |
| RSS | 40–70 MB（随输出尺寸增长） |
| 私有（匿名）内存 | **6–25 MB**（随封面缓存与 cairo/pango 字形缓存增长） |
| CPU | **约 5.5% 单核**（`460x690` @30fps，其中封面自转占 3.5 个百分点） |
| 无消费者连接时 | 0 帧推送，CPU **约 0.25%** |
| 子进程 | **0**（常驻 D-Bus 连接） |

私有内存之外的部分是 cairo / pango / dbus / curl / gdk-pixbuf / PipeWire 等共享库与 fontconfig 缓存，属于系统级共享页，不会与其它进程重复占用。

### 尺寸选择

版面是「圆封面 + 下方文字」的纵向堆叠，**圆直径受纵向预算约束**，所以方形画布左右必然空掉一截。把宽度收窄到刚好包住内容即可消除留白：

| `--size` | 内容占宽 | 左右各余 | CPU (单核) |
| --- | --- | --- | --- |
| `540x540` | 81% | 19% | ~4.1% |
| `360x540` | 96% | 4% | ~3.6% |
| `400x600` | 96% | 4% | ~4.2% |
| **`460x690`** | **96%** | **4%** | **~5.5%** |
| `480x720` | 96% | 4% | ~6.3% |

放大时**宽高同比缩放**、保持 `W:H = 2:3`，左右留白恒为 4%。开销主要由封面和文字决定，两者都随**高度**增长，所以收窄宽度几乎不省 CPU——收窄只为消除空白。

### 进一步降低 CPU

- `--spin 0`：直接降低约 60%（自转是逐帧重采样，为全流程开销最大的环节）。
- `--fps 24`：约省六分之一。
- 把尺寸高度调小一档。

## 排查

| 现象 | 检查项 |
| --- | --- |
| Source 下拉框中没有该节点 | 进程是否在运行；对话框是否在进程启动前就已打开（节点只在打开对话框时枚举一次）；`media.type` / `media.class` / `media.role` 三项属性是否齐全 |
| 下拉框中有节点，但 OBS 中一片空白 | 流标志中是否误加了 `PW_STREAM_FLAG_DRIVER`（见 [docs/internals.md](docs/internals.md)） |
| 画面被拉伸或裁切 | OBS 中源的宽高是否与 `--size` 一致 |

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `README.zh-CN.md` | 本文件（中文版） |
| `README.md` | 英文版（默认） |
| `LICENSE` | MIT 许可证全文 |
| `docs/internals.md` | 架构说明与实机踩坑记录（改代码前先读） |
| `docs/card-*.png` | `--dump` 输出的效果图 |
| `Makefile` | 构建脚本，含 `dump` / `run` / `install-service` / `uninstall-service` 目标 |
| `pw-mpris-visualcard.service` | systemd 用户服务模板，由 `make install-service` 渲染安装 |
| `src/types.hpp` | `Track` / `NowPlaying` / `Config` 数据结构 |
| `src/mpris.{hpp,cpp}` | sdbus-c++ 常驻连接 + 采样线程 + LRC 解析 |
| `src/art.{hpp,cpp}` | libcurl 抓图 → gdk-pixbuf 解码 → cairo 表面（小型 LRU） |
| `src/card.{hpp,cpp}` | cairo + pango 版面绘制 |
| `src/pwvideo.{hpp,cpp}` | libpipewire 视频节点输出 |
| `src/main.cpp` | 模块组装与命令行解析 |

## 修改代码

先读 [docs/internals.md](docs/internals.md)。其中记录了 8 条实机验证得到的约束，包括一个**静默失效**的 PipeWire 标志组合，和一个会**破坏红色通道**的位运算技巧——两者都耗费了不少时间才定位。

## LLM 参与开发

**本项目在 LLM 辅助下开发**，代码与文档都有 LLM 的产出。

这带来一个直接的方法论要求：文档里的每个技术结论都必须能实机复现。`docs/internals.md` 里的数字（0.431 ms 的旋转、7.5Hz 的缓存刷新率、整帧 -22%……）全部来自本机测量，不是模型给出的估计；无法复现的说法不写进文档。这条规则对模型和对人一视同仁——所以文档里的表格才要保留 A/B 对照与复现命令。

## 欢迎 LLM 贡献

由 LLM 生成或辅助生成的 patch 同样欢迎，评审只看证据，不看作者是人还是模型。为使评审可行，请满足：

- **附实测证据**：改变行为的改动给出复现命令与输出；涉及性能的改动附微基准代码与数据。改热循环前先写微基准，[docs/internals.md](docs/internals.md) 里"三剪切更慢"和"定点快 43%"都是这么得出的。
- **不写入未经验证的数字**：没有测量来源的性能数据不要进正文，宁可不写。
- **遵守已记录的约束**：上面那 8 条硬性要求是踩坑换来的，与之相悖的方案必须先拿出 A/B 证据，而不是先改代码。
- **提交信息注明协作方式**（建议非必须）：例如 `Co-authored-by: <model>`，或说明哪部分由模型产出，便于日后回溯。
- **责任在提交者**：审核、验证与后续维护由提交者承担，LLM 无法承担这些。

不接受：没有复现路径的性能声明、仅凭直觉的"优化"、为了让文字更顺口而丢掉前提条件的文档改写。

本项目没有 CLA，也不要求给 LLM 署名。

## 许可证

本项目采用 **MIT License**，著作权归 **ZokuTe**（2026 年起），全文见 [LICENSE](LICENSE)。提交的贡献按同一许可进入本项目。

运行期以动态链接方式使用以下系统库，未捆绑、未修改其代码，各自适用其自身许可：

| 库 | 本机已装版本声明的许可 |
| --- | --- |
| cairo | LGPL-2.1-only OR MPL-1.1 |
| pango | LGPL-2.0-or-later |
| gdk-pixbuf | LGPL-2.0-or-later |
| PipeWire | MIT，LGPL-2.1-or-later |
| sdbus-c++ | LGPL-2.1-only，含 sdbus-c++ LGPL 例外条款 |
| libcurl | curl 许可（MIT 类） |

OBS 侧的 [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 是 GPLv2 的独立程序，与本项目之间只有 PipeWire 节点的运行时数据流，不构成链接或派生关系，故各自许可互不影响。

