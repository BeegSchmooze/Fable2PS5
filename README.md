# Fable 2 for PS5

An unofficial native port of **Fable II** (Xbox 360, 2008) to a jailbroken PlayStation 5.

The game's Xbox 360 executable is statically recompiled to C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (a Xenia-derived recompiler and runtime),
built for the PS5 with the [ps5-payload-dev](https://github.com/ps5-payload-dev) SDK, and drawn
with RADV, Mesa's Vulkan driver ported to the console. It runs from the home screen as a folder
title named **Fable 2**, at a 2560×1440 internal resolution with FXAA, capped at 30 FPS.

> **No game content is included, and none ever will be.** You need your own copy of the game.
> The build regenerates all game-derived code from *your* `default.xex` on *your* machine.
> This project is not affiliated with or endorsed by Microsoft, Xbox Game Studios, Lionhead,
> or Sony Interactive Entertainment. "Fable" is a trademark of Microsoft.

---

## Contents

- [Status](#status)
- [What you need](#what-you-need)
- [Building](#building)
- [Installing on the PS5](#installing-on-the-ps5)
- [Playing](#playing)
- [Known issues](#known-issues)
- [Improvements over a straight recompile](#improvements-over-a-straight-recompile)
- [Tried and rejected](#tried-and-rejected)
- [How the build works](#how-the-build-works)
- [Troubleshooting](#troubleshooting)
- [Licences and credits](#licences-and-credits)

---

## Status

Playable. It has been played through the opening chapters, Bowerstone (market, tavern, cellar),
Bloodstone and the main-menu/save flow, with saving and loading working.

| Area (standing or walking) | Frame rate |
| --- | --- |
| Indoors, quiet areas | 30 (the cap) |
| Bowerstone streets | 26–30 |
| Bloodstone (the heaviest place measured) | about 24 |

The limit is CPU-side. One emulator thread turns the game's ~6,000–10,000 draw calls per frame
into Vulkan commands at about 5.3 µs each, so busy scenes run below 30. The GPU has headroom to
spare, which is why 1440p costs nothing. See [Known issues](#known-issues).

## What you need

**A PS5 with a homebrew environment.** The port was developed and tested with:

- [etaHEN](https://github.com/etaHEN/etaHEN) as the homebrew enabler,
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) to register folder titles under
  `/data/homebrew` on the home screen,
- an FTP server on the console (for example [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv)) to
  copy files over.

This repository does not set any of that up. Follow those projects' own instructions, and keep
the services on a trusted local network.

**Your own Fable II GOTY game files** (Xbox 360), dumped from a disc you own:

- `default.xex`, which must be this exact build of the game:

  | | |
  | --- | --- |
  | Title ID | `4D5307F1` |
  | Media ID | `716F0A0D` |
  | Version | `0.0.0.26` |
  | SHA-256 | `88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662` |

  The build stops if the hash differs. The recompiled code and this port's patches only fit that
  executable.
- the game's `data/` folder (about 6.5 GB). It goes on the console, not into the build.

**A PC with Docker** (x86-64, Linux, or Windows/macOS with Docker Desktop). Plan for about 40 GB
of free disk and 16 GB of RAM. More cores make the build much faster.

## Building

```bash
git clone <this repository> fable2-ps5
cd fable2-ps5
cp /path/to/your/default.xex game/
cp /path/to/your/cover.png game/      # optional: your own image for the home-screen icon

docker build -t fable2-ps5 .          # once, about an hour (Mesa/RADV is most of it)
docker run --rm -v "$PWD/game:/game:ro" -v "$PWD/out:/out" fable2-ps5
```

On Windows PowerShell, use `${PWD}` instead of `$PWD`.

The run takes 10–30 minutes, depending on cores, and writes the finished title folder:

```
out/PPSA99124/
  eboot.bin              the game (signed, ~145 MB)
  sce_sys/param.json     title "Fable 2"
  sce_sys/icon0.png      512×512, made from game/cover.png (or a plain placeholder)
  sce_module/libc.prx
  cache-seed/            shipped shader-cache data (see "Shader cache" below)
```

`docker run ... fable2-ps5 --help` lists the options (`--jobs N`, `--skip-xex-check`).

**About the cover art.** The repository ships no box art because it belongs to Microsoft. Put any
image you own at `game/cover.png`. The build fits it into a 512×512 icon over a blurred copy of
itself, without cropping or stretching.

## Installing on the PS5

1. Copy `out/PPSA99124/` to the console as `/data/homebrew/PPSA99124/`.
2. Copy your game files **into that same folder**:
   - `default.xex` → `/data/homebrew/PPSA99124/default.xex`
   - `data/` → `/data/homebrew/PPSA99124/data/`

   The port looks for both beside `eboot.bin` and shows an error if either is missing.
3. Let ShadowMountPlus pick the folder up. A **Fable 2** tile then appears on the home screen.

The finished folder looks like this:

```
/data/homebrew/PPSA99124/
  eboot.bin  sce_sys/  sce_module/  cache-seed/      from out/PPSA99124
  default.xex  data/                                 your game files
  saves/  cache/  logs/                              created by the game
```

**Updating** to a newer build: close the game completely, then copy the new `out/PPSA99124/` over
the old one. Your `saves/` and `cache/` stay. If the home-screen name or icon doesn't change,
delete the tile from the home screen. It comes straight back with the new name and icon. Your
files on the console are not affected.

## Playing

- **Controls** follow the usual Xbox-to-PlayStation layout (Cross = A, Circle = B, Square = X,
  Triangle = Y). On-screen prompts still show Xbox buttons.
- **Saves** live in `/data/homebrew/PPSA99124/saves/`, one folder per hero slot. Back that folder
  up before experimenting. Nothing in this project touches it.
- **Quitting:** use the game's own menu, or close it from the PS5 menu.

### Shader cache

A new shader or render state compiles the first time the game meets it. That is the brief hitch
when you enter a new area. Everything compiled is remembered in
`/data/homebrew/PPSA99124/cache/shaders/shareable/` and built ahead of time on every later start.

To spare new players most of those hitches without shipping any of the game's code, the title
carries `cache-seed/`:

- `4D5307F1.fbo.vk.xpso`: about 940 pipeline descriptions recorded in play (render states and shader
  hashes only). On each start they are merged into your cache.
- `4D5307F1.shader-index.bin`: a hash-only list of 259 shaders. The shaders themselves are game
  code, so they are not shipped. Instead, a low-priority thread scans the game's memory a few
  times in your first sessions (1, 3, 8, 18, 38 and 78 minutes after start), finds those shaders in
  *your* running copy by hash, and saves them to `4D5307F1.recovered.xsh`. On the next start they
  join your cache, so their pipelines are built at startup instead of mid-game.

The console log shows progress as `[FABLE2-SHADERS] ...` lines. The cache keeps growing as you
play, as it always does.

## Known issues

| Issue | Details |
| --- | --- |
| **Below 30 FPS in busy towns** | Bloodstone runs at about 24, Bowerstone's busiest streets at 26–28. The emulator's GPU command thread is CPU-bound (see [Status](#status)). The remaining big costs are descriptor-set updates (~15% of that thread), the page-change checks that keep memory coherent (~13%) and texture lookups (~9%). Getting Bloodstone to 30 needs roughly 25% more throughput, which means reworking those three areas. |
| **Loading-screen picture is a block of random colours (or black)** | The area artwork on loading screens fails to render. Builds 118–120 traced it to a 640×360 resolve copy that reads back as zeros; the picture is texture `17fc0000` (1024×512 DXT5). It has been present since before any of the performance work. Cosmetic. |
| **Item pickup card shows a black background** | Where the Xbox 360 shows the dimmed world behind the "Take / Use / Info" card, the background is pure black (exactly 0,0,0). It is most likely the same "resolve copy reads zero" problem as the loading screens. Cosmetic. |
| **Hitch the first time an area is seen** | Shader and pipeline compiles (44 of 97 hitches over 100 ms in one session were these). The shipped cache seed and shader recovery remove most of them after the first session, see [Shader cache](#shader-cache). Some hitches are the game's own streaming. |
| **Dog briefly drawn red** | Seen once when entering the cellar behind the Bowerstone tavern. Not seen again since the build-116 memory fix. |
| **Codegen warnings** | The recompiler reports one unresolved branch (`0x83040DB8`), and Fable-2-Recomp documents a few unresolved conditional branches. Those paths stop with `REX_FATAL` if ever reached; none has been hit in play. |
| **Diagnostic log lines** | The build still prints performance counters to the console log every 10 s (`FABLE2-PERF`, `LOCK-PERF`, `TEX58`, ...). They are cheap and useful for bug reports. |
| **One game build only** | Other versions or regions of `default.xex` are refused (see [What you need](#what-you-need)). |

## Improvements over a straight recompile

Highlights from about 170 test builds (numbers are the development build where each change landed):

**Stability and correctness**

- **Story-moment hangs fixed (62).** On the 360 the frame worker always finished before the main
  thread took critical section `0x83499138`. On the PS5's scheduler it sometimes didn't, and both
  threads blocked forever. The main thread now releases that lock while it waits for the worker,
  then retakes it to the same depth.
- **Menu flicker and ground flashing fixed (116).** For memory pages the game rewrites constantly,
  the emulator compares the exact requested bytes against a shadow copy on every request, instead
  of trusting a page until the next frame.
- **Main-menu music restored (113).** The menu track resolved with a sample gain of 0.02, which is
  nearly silent. It now plays at 1.0, and no other sound is touched.
- **No time limit (170).** The development harness quit the game after four hours. That is gone.

**Speed** (Bloodstone, standing: 20.5 → 23.8 FPS from 161 to 169)

- **Native atomics (30–33).** 8,371 guest compare-and-swap blocks run as single host atomics
  instead of taking the runtime-wide lock.
- **Cheap timers (54).** Always-on probes read a calibrated TSC instead of `steady_clock`, which
  costs about 1 µs per read on the PS5.
- **Probe removal (126)** and a **sampler-parameter memo (137)**, which hits 96% of the time and
  cuts per-draw cost by 5%.
- **AVX2 page compare (146)**, another 5% per draw.
- **Asynchronous submission (167).** Replaying the recorded commands into RADV and submitting them
  now runs on its own thread. The draw thread waits for it only before a swap or a fence check.
  Submit time on the draw thread fell from 1.2 s to 0.2 s per 10 s.

**Presentation**

- **2× resolution (121):** 2560×1440 internally, free at the 30 FPS cap.
- **FXAA (157)** on the final image.
- **Shader cache seeding and recovery (170)**, see [Shader cache](#shader-cache).
- Home-screen title **Fable 2** with an icon made from your own cover image.

## Tried and rejected

These are recorded so nobody has to repeat them:

- **60 FPS:** not reachable. Even uncapped, busy streets top out at 27–38 FPS because the command
  thread is CPU-bound. Pinning that thread to a core or raising its priority made no difference.
- **Frame generation:** not a PS5 system feature, and it would need motion vectors the game
  doesn't produce.
- **4K internal resolution (3×):** 19–25 FPS.
- **"FXAA extreme":** about 20 FPS.
- **Submitting only once per frame:** about 22 FPS. The GPU starts too late and frames slip past
  33 ms.
- **Submitting only after 800+ draws:** fewer slow frames, but it caused a persistent black
  world, with only the HUD still drawn. Never ship it.
- **Re-checking all page state every frame** (`clear_memory_page_state`): 15× more uploads, heavy
  stutter.
- **Skipping texture key rebuilds** and **reusing identical descriptor sets:** both measurable in
  isolation, with no frame-rate change.

## How the build works

The port grew as incremental, object-by-object experiments, so the build replays a **recorded
recipe** rather than running a conventional build system. Every step is verified:

1. **`docker build`** installs the exact compilers from Ubuntu's snapshot archive (clang 18.1.3 and
   19.1.1). It clones the pinned commits of each source repository (`docker/pins.sh`), applies
   `patches/rexglue-sdk.patch` to ReXGlue, and copies `overlay/` (the port's own sources, at the
   paths the recipe expects). It then builds RADV, both payload-SDK pins, FFmpeg's WMA Pro/XMA
   decoders, the ELF signing tool and the `rexglue` host tool.
2. **`docker run`** checks your `default.xex`, then runs `rexglue codegen` on it. Codegen is
   deterministic: the same executable gives the same 600 files.
3. **`recomp/transform.py`** applies this port's changes to the generated code (atomics, the hang
   fix, the music fix, probes). It holds only patterns and guest addresses, never game code, and
   it stops if the code doesn't match what it expects.
4. **`recipe/compile.json`** holds the exact command for each of the 588 objects. They were
   recovered from the development tree and **verified by rebuilding all of them byte for byte**.
5. The objects are linked with PS5_Vulkan's recipe and RADV in the order in
   `recipe/link-inputs.txt`. The result is signed and packaged by `tools/package.py`.

A run of this container with only `default.xex` as input produced an `eboot.bin` identical to
the developer's tested build (sha256 `7264ba869470ba58…`). If your build prints that hash,
you are running exactly what was tested.

Maintainer tools: `tools/export_repo.py` regenerates `recipe/`, `overlay/` and the patch from a
development workspace. `recomp/build_transform.py` regenerates `transform.py` from its template.
`tools/xecache_merge.c` validates and merges cache files, and `index` makes the hash-only shader
list from a `.xsh`.

## Troubleshooting

- **"default.xex SHA-256 is …"**: you have a different build of the game; see
  [What you need](#what-you-need).
- **"default.xex was not found next to …" / "The data folder was not found …" on the console**:
  copy your game files into `/data/homebrew/PPSA99124/` (see
  [Installing](#installing-on-the-ps5)).
- **The tile still shows the old name or icon**: delete the tile from the home screen. It
  reappears updated.
- **Console log:** with a klog server running, the boot prints
  `[FABLE2-BOOT] OPTIONS sync_shaders=1,1 keep_page_state=1 scale2x=1 fxaa=1`. After that come
  `[FABLE2-PERF] ... FPS=` lines every 10 s, which are useful in bug reports.

## Licences and credits

This repository is under the **GNU GPL v3** (`LICENSE`), because the title links the payload SDK
fork's GPL-3.0 platform layer. Components keep their own licences:

| Component | Author | Licence |
| --- | --- | --- |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (recompiler and runtime, derived from [Xenia](https://xenia.jp)) | Tom Clay; Ben Vanik and Xenia contributors | BSD 3-Clause |
| [Fable-2-Recomp](https://github.com/himdo/Fable-2-Recomp) (recompilation project, manifest, game-specific code) | Himdo | MIT |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (RADV), [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK), PS5 Vulkan Template | Mihawk-99, on Mesa, ps5-payload-dev and Sascha Willems' Vulkan examples | GPL-3.0 / MIT |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | John Törnblom and contributors | GPL-3.0 |
| FFmpeg (WMA Pro / XMA decoders) | FFmpeg developers | LGPL-2.1+ |

The port's own code is `port/`, `recomp/`, `tools/`, `docker/` and the files under `overlay/`.
Thanks also to etaHEN and ShadowMountPlus for the homebrew environment it runs in.

Fable II © Microsoft Corporation. This project contains no Microsoft code or assets. The
shipped cache seed holds only render-state descriptions and hashes.
