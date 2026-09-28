# pw-mpris-visualcard

[English](README.md) · [简体中文](README.zh-CN.md)

Renders whatever music is playing on this machine as a card and publishes it to OBS directly as a **PipeWire video node**.

No browser, no CEF, no HTTP: one process, no child processes, and nothing is rendered while no consumer is connected.

## How it works

| Stage | Input | Output | Implementation |
| --- | --- | --- | --- |
| Playback state | The player's MPRIS interface (D-Bus) | A `NowPlaying` snapshot: title, artist, album, position, lyrics, cover URL | `mpris`, one persistent D-Bus connection, no forked processes |
| Artwork | Cover URL | cairo surface (LRU cache, up to 3 entries) | `art`, background thread + libcurl + gdk-pixbuf |
| Layout | Snapshot + cover surface | BGRA frame (premultiplied alpha) | `card`, cairo + pango |
| Video output | BGRA frame | `Stream/Output/Video` node | `pwvideo`, libpipewire |

Every stage runs in the same process and passes data in memory; private memory stays at 6–25 MB.

## Features

- Playback metadata comes from D-Bus / MPRIS (musicfox, Spotify, VLC, mpv, Rhythmbox, …) over a single persistent connection and with zero child processes; polling implementations fork `busctl` several times per second.
- Fully transparent background by default: only the cover disc and the text are on screen. Both carry their own drop shadow, so they stay legible over bright content.
- Synced lyrics taken from the MPRIS `xesam:asText` property (LRC). The current line keeps the first slot and is highlighted, the following lines sit below it, and the block never jumps.
- The cover rotates like a record and freezes while paused.
- Progress ring, elapsed time and album name can be toggled independently.
- Any output size (`WxH`); the layout scales proportionally with height.
- Adjustable frame-rate ceiling; consumers may negotiate a lower rate, never a higher one.
- The layout can be tuned without opening OBS: `--dump` writes a PNG directly.

## Preview

Both PNGs below are `--dump` output captured during real playback. The background is transparent; nothing was retouched.

![Example card](docs/card-460x690.png)

`--size 460x690 --lyrics 4 --time 1 --album 1`: circular cover (rotating) + title + artist · album + highlighted current line + dimmed following lines + progress ring + `0:38 / 2:47`.

With the default options (`360x360`, no lyrics / time / album) you get the minimal form:

![Default card](docs/card-360x360.png)

## Getting started

### 1. Dependencies

Everything comes from the distribution repositories; no language package manager is involved.

```bash
# Arch
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire sdbus-cpp curl
```

On the OBS side you need a plugin that can select an arbitrary PipeWire node. OBS ships `linux-pipewire`, which goes through xdg-desktop-portal and can only capture screens and windows — it cannot see this node. Use the [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) plugin instead.

### 2. Build

```bash
make             # → ./pw-mpris-visualcard-native
make PORTABLE=1  # the same, without -march=native (other machines, redistribution)
```

The default flags are `-O3 -march=native -funroll-loops`: the cover-rotation hot loop benefits measurably, cutting 22% off a whole frame. The trade-off is a binary tied to the local instruction set, which is what `PORTABLE=1` exists for.

### 3. Run

```bash
./pw-mpris-visualcard-native
```

The node name (default `pw-mpris-visualcard`) is printed to the terminal.

### 4. Add it to OBS

1. Sources **+** → **PipeWire Video** (provided by `obs-pwvideo`).
2. Pick **Music Card** in the “Source” dropdown. The dropdown displays the node description (`--desc`, default `Music Card`), while the node it actually connects to is `--node` (default `pw-mpris-visualcard`). Both are configurable; do not go by the label alone.
3. **Set width and height to match `--size`** (default `360 × 360`) so the card is shown 1:1 and unscaled.

The alpha channel passes through unchanged, so the card can be overlaid on the scene as is.

> Node missing from the dropdown? The plugin enumerates nodes once, **at the moment the dialog is opened**, and never refreshes an open window: confirm the process is running, then reopen the properties window. The node must also satisfy all three of `media.type=Video`, `media.class=Stream/Output/Video` and `media.role=Production` — one missing and it never shows up. Details in [docs/internals.md](docs/internals.md) (Chinese only for now).

### 5. Autostart (optional)

The `pw-mpris-visualcard.service` unit in this repository is a **template** holding two placeholders, `@REPO@` (absolute path to the checkout) and `@ARGS@` (launch arguments), so **copying it verbatim will not work**: systemd rejects the unit at load time as misconfigured (`WorkingDirectory= path is not absolute: @REPO@`) and never starts it. Render and install it with `make`:

```bash
make install-service                            # render the unit into ~/.config/systemd/user/
systemctl --user enable --now pw-mpris-visualcard    # enable and start it
```

The default arguments are `--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3`; override `SERVICE_ARGS` to change them:

```bash
make install-service SERVICE_ARGS="--node pw-mpris-visualcard --size 360x360 --fps 30"
systemctl --user restart pw-mpris-visualcard         # restart to apply new arguments
```

`make install-service` only writes the unit and runs `daemon-reload`; it never enables or starts anything for you, so the existing state of the machine is left alone. To remove the unit:

```bash
make uninstall-service
```

> Installing by hand works too, but both placeholders must be replaced first. Note also that the service sets `PrivateTmp=yes`, so `--dump` output would land in the service's private `/tmp` and be unreachable — tune the layout by running `--dump` in a terminal, not from the unit.

## Command-line options

| Option | Default | Description |
| --- | --- | --- |
| `--size WxH` | `360x360` | Output size; a single number means a square. **The entire layout scales with height, width only sets the side margins** — the circular cover is constrained by the vertical budget, so a square canvas always leaves a wide margin |
| `--fps N` | `30` | **Frame-rate ceiling.** The range advertised to PipeWire is `[N/4, N]`; consumers may negotiate lower but never higher. Frames are pushed at the negotiated rate, floored at 5fps |
| `--bg MODE` | `none` | `none` is fully transparent, `solid` is an opaque dark background, `#rrggbb` sets a specific colour |
| `--progress 0\|1` | `1` | Progress ring |
| `--time 0\|1` | `0` | Show `1:23 / 3:12` |
| `--album 0\|1` | `0` | Append the album name after the artist |
| `--lyrics N` | `0` | Number of lyric lines; `0` disables them |
| `--spin SEC` | `24` | Seconds per full cover rotation; `0` disables rotation |
| `--idle last\|hide` | `hide` | Keep the last track on screen after playback stops |
| `--node NAME` | `pw-mpris-visualcard` | PipeWire node name; this is the value behind the OBS dropdown entry. Duplicate names get an `(id)` suffix to tell them apart |
| `--desc TEXT` | `Music Card` | Node description. **This is what the OBS dropdown displays**, not `--node` |
| `--verbose`, `-v` | | Log negotiation, frame pushes and per-frame timings |
| `--dump FILE` | | Render one sample to a PNG and exit |
| `--demo` | | Use fake data; do not connect to D-Bus |
| `--help`, `-h` | | Print a short option summary |

### Tuning the layout without opening OBS

```bash
make dump                                              # one PNG from fake data
./pw-mpris-visualcard-native --dump /tmp/card.png --time 1  # from real playback data
./pw-mpris-visualcard-native --demo --dump /tmp/card.png --lyrics 4 --time 1 --album 1
```

## Performance

| Metric | Value |
| --- | --- |
| RSS | 40–70 MB (scales with output size) |
| Private (anonymous) memory | **6–25 MB** (grows with the cover cache and the cairo/pango glyph caches) |
| CPU | **≈5.5% of one core** (`460x690` at 30fps, of which cover rotation accounts for 3.5 points) |
| With no consumer attached | zero frames pushed, **≈0.25%** CPU |
| Child processes | **0** (persistent D-Bus connection) |

What lies beyond the private memory is shared libraries (cairo, pango, dbus, curl, gdk-pixbuf, PipeWire) and fontconfig caches — system-wide shared pages that are not duplicated per process.

### Choosing a size

The layout stacks a circular cover above the text, and **the circle's diameter is constrained by the vertical budget**, so a square canvas inevitably leaves margins on both sides. Narrowing the width until it just fits the content removes them:

| `--size` | Content width | Side margins | CPU (one core) |
| --- | --- | --- | --- |
| `540x540` | 81% | 19% | ~4.1% |
| `360x540` | 96% | 4% | ~3.6% |
| `400x600` | 96% | 4% | ~4.2% |
| **`460x690`** | **96%** | **4%** | **~5.5%** |
| `480x720` | 96% | 4% | ~6.3% |

When scaling up, multiply **both dimensions** and keep `W:H = 2:3`; the side margins stay at 4%. Cost is dominated by the cover and the text, both of which scale with **height**, so narrowing the width saves almost no CPU — it only removes empty space.

### Reducing CPU further

- `--spin 0` — cuts roughly 60% outright (rotation resamples every frame and is the most expensive stage in the pipeline).
- `--fps 24` — saves about one sixth.
- Drop the height by one step.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| The node is absent from the Source dropdown | Is the process running? Was the dialog opened before it started (nodes are enumerated only when the dialog opens)? Are all three properties `media.type`, `media.class` and `media.role` present? |
| The node is listed but OBS shows a blank frame | Whether `PW_STREAM_FLAG_DRIVER` was added to the stream flags (see [docs/internals.md](docs/internals.md)) |
| The image is stretched or cropped | Whether the source width and height in OBS match `--size` |

## Repository layout

| Path | Contents |
| --- | --- |
| `README.md` | This file (English) |
| `README.zh-CN.md` | Chinese version |
| `LICENSE` | Full text of the MIT license |
| `docs/internals.md` | Architecture notes and field-tested pitfalls (read before changing code; Chinese only for now) |
| `docs/card-*.png` | Example output from `--dump` |
| `Makefile` | Build script with the `dump` / `run` / `install-service` / `uninstall-service` targets |
| `pw-mpris-visualcard.service` | systemd user service template, rendered by `make install-service` |
| `src/types.hpp` | `Track` / `NowPlaying` / `Config` data structures |
| `src/mpris.{hpp,cpp}` | sdbus-c++ persistent connection + sampling thread + LRC parsing |
| `src/art.{hpp,cpp}` | libcurl fetch → gdk-pixbuf decode → cairo surface (small LRU) |
| `src/card.{hpp,cpp}` | Layout rendering with cairo + pango |
| `src/pwvideo.{hpp,cpp}` | libpipewire video node output |
| `src/main.cpp` | Module wiring and command-line parsing |

## Modifying the code

Read [docs/internals.md](docs/internals.md) first (Chinese only for now). It records eight constraints established on real hardware, including a **silently failing** combination of PipeWire stream flags and a bit-twiddling trick that **destroys the red channel** — both took a long time to pin down.

## LLM involvement

**This project is developed with LLM assistance**; both the code and the documentation contain LLM output.

That imposes a straightforward methodological rule: every technical claim in the documentation must be reproducible on real hardware. The figures in `docs/internals.md` (0.431 ms per rotation, the 7.5Hz cache refresh rate, 22% off a whole frame, …) all come from measurements on this machine rather than from model estimates, and claims that cannot be reproduced do not make it into the documentation. The rule applies equally to models and to humans — which is why the tables retain their A/B comparisons and reproduction commands.

## LLM contributions welcome

Patches written or assisted by an LLM are welcome; review looks at the evidence, not at whether the author is a human or a model. To keep review tractable:

- **Include measured evidence.** For behaviour changes, give the command and its output; for performance changes, include the microbenchmark code and data. Write the microbenchmark before touching a hot loop — that is how the “three-shear is slower” and “fixed-point is 43% faster” results in [docs/internals.md](docs/internals.md) were established.
- **Do not add unverified numbers.** Performance figures with no measurement behind them do not belong in the documentation; better to leave them out.
- **Respect the documented constraints.** Those eight hard requirements were paid for in debugging time; a proposal that contradicts one needs A/B evidence first, not a code change first.
- **State how the work was produced** (recommended, not required) — for example `Co-authored-by: <model>`, or a note on which parts were generated, so it can be traced later.
- **The submitter is responsible.** Review, verification and long-term maintenance rest with the submitter; an LLM cannot carry any of that.

Not accepted: performance claims with no reproduction path, “optimisations” based on intuition alone, and documentation rewrites that drop preconditions to read more smoothly.

There is no CLA, and no LLM attribution is required.

## License

Released under the **MIT License**, copyright **ZokuTe** (from 2026); see [LICENSE](LICENSE) for the full text. Contributions enter the project under the same license.

The following system libraries are used at runtime through dynamic linking. None of their code is bundled or modified, and each remains under its own license:

| Library | License declared by the installed package |
| --- | --- |
| cairo | LGPL-2.1-only OR MPL-1.1 |
| pango | LGPL-2.0-or-later |
| gdk-pixbuf | LGPL-2.0-or-later |
| PipeWire | MIT, LGPL-2.1-or-later |
| sdbus-c++ | LGPL-2.1-only, with the sdbus-c++ LGPL exception |
| libcurl | curl license (MIT-style) |

[obs-pwvideo](https://github.com/tasokait/obs-pwvideo) on the OBS side is a separate GPLv2 program. Its only interaction with this project is the runtime data flow through a PipeWire node — no linking, no derivative work — so the two licenses do not affect each other.
