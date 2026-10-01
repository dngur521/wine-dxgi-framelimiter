# wine-dxgi-framelimiter

**[한국어 README](README.ko.md)**

A low-latency frame limiter for **Windows games running in Wine + [DXMT](https://github.com/3Shain/dxmt) on macOS**.
Cap a game at 60 fps (or any value) to save power and heat on a high-refresh display — **without the mouse lag** that DXMT's own limiter adds, and without touching Steam or any other program in the prefix.

```
./setup.sh
./flwine --fps 60 --exe Game.exe "C:\Program Files (x86)\Steam\Steam.exe"
```

## Why

| Option | Problem |
|---|---|
| In-game 60 fps option | Often just VSync — on a 120/240 Hz display it doesn't cap at 60, or burns resources |
| DXMT `DXMT_CONFIG="d3d11.preferredMaxFrameRate=60"` | Paces at the **end** of the pipeline (`presentDrawable:afterMinimumDuration:`). The game keeps rendering ahead, DXMT's 3-frame queue and the drawable pool fill up → **~50–80 ms input lag**: the mouse visibly trails |
| Metal-layer limiters (e.g. hooking `-[CAMetalLayer nextDrawable]`) | Runs on DXMT's encode thread, still behind the 3-frame queue |

This project instead waits **on the game's own render thread, right after `Present()`** — the same idea as DXVK's `dxgi.maxFrameRate` or RTSS. The game wakes, reads fresh input, renders, and that frame is shown immediately. The queue never fills, so input latency stays at about one frame. The macOS cursor is drawn by WindowServer and is never affected.

## How it works

```
Game.exe ──► system32\dxgi.dll  (this proxy)  ──► system32\dxgi_orig.dll  (DXMT's real dxgi)
                 │
                 ├─ forwards CreateDXGIFactory / 1 / 2 / DXGIGetDebugInterface1
                 ├─ patches the factory's CreateSwapChain* methods
                 └─ patches IDXGISwapChain::Present / Present1:
                        call the real Present, then sleep until the next frame slot
```

- Only active when `FL_FPS` is set **and** the process name matches `FL_EXE` — otherwise a pure passthrough.
- Sleeps with `NtDelayExecution` (no busy-wait). Deadlines accumulate (no drift) and resync after loading screens / alt-tab (no catch-up burst).

## Requirements

- macOS with Wine (tested: Wine Stable 11.0, x86_64 under Rosetta on Apple Silicon)
- **DXMT installed in the prefix** (`system32\dxgi.dll` must be DXMT's, with the `dxgi` DLL override set to `native,builtin` — DXMT's installation does this)
- 64-bit D3D11 game (D3D12 via DXMT should work too but is untested)
- Optional: `brew install mingw-w64` to build from source. Without it, `setup.sh` downloads the prebuilt `dxgi.dll` from the [latest release](../../releases/latest).

## Tested setup

| | |
|---|---|
| Mac | MacBook Pro, Apple M5 (Mac17,2) |
| macOS | 27.0.1 |
| Display | External 1920×1080 @ **240 Hz** (main) |
| Wine | Wine Stable 11.0 (`/Applications/Wine Stable.app`, x86_64 via Rosetta), prefix `~/.wine` |
| Graphics | DXMT (`d3d11`/`dxgi` = `native,builtin`) |
| Steam | Windows Steam inside Wine, launched with `-no-cef-sandbox -vgui -silent` from an Automator app |
| Game | **Blue Archive** (Steam, Unity, D3D11) — `BlueArchive.exe` |

Result in Blue Archive at `FL_FPS=60`: a steady 60 fps, the mouse follows with no noticeable lag (unlike `d3d11.preferredMaxFrameRate=60`), GPU utilisation ≈ 40%. Steam itself stays uncapped.

Tried another game? Please open an issue or PR to add it to this table.

## Install (one click)

```sh
git clone https://github.com/dngur521/wine-dxgi-framelimiter.git
cd wine-dxgi-framelimiter
./setup.sh                        # default prefix: $WINEPREFIX or ~/.wine
./setup.sh --prefix ~/my-prefix   # another prefix
```

`setup.sh` builds (or downloads) the proxy, checks the prefix really contains DXMT, keeps the original as `system32\dxgi_orig.dll` (plus a timestamped copy in `./backup/`), and installs the proxy as `system32\dxgi.dll`. Safe to re-run.

## Usage

**1. Find the game's exe name** — e.g. `BlueArchive.exe`. While the game runs:

```sh
ps -axo comm | grep -i '\.exe'
```

**2. Quit Steam completely** (and every other Wine program in the prefix). Environment variables only reach the game when Wine starts fresh; a game started by an already-running Steam won't see them.

**3. Launch through `flwine`:**

```sh
./flwine --fps 60 --exe BlueArchive.exe "C:\Program Files (x86)\Steam\Steam.exe" -silent
```

…or add the variables to your existing launch script before the `wine` line:

```sh
export FL_FPS=60
export FL_EXE=BlueArchive.exe
# export DXMT_CONFIG="d3d11.preferredMaxFrameRate=60;"   # old way — remove, it adds input lag
wine "C:\Program Files (x86)\Steam\Steam.exe" -silent &
```

**4. In the game:** set its frame rate option to the maximum/unlimited and turn VSync off, so the game's own limiter doesn't fight this one.

### Example: macOS app launcher (Automator)

Automator → New → Application → *Run Shell Script*:

```sh
export FL_FPS=60
export FL_EXE=BlueArchive.exe
wine "C:\Program Files (x86)\Steam\Steam.exe" -silent > /dev/null 2>&1 &
```

## Not sure how? Ask an AI assistant

If you use Claude Code, Codex, Cursor or another AI agent that can run terminal commands on your Mac, paste this (fill in the brackets):

```text
I want to cap a Windows game running in Wine + DXMT on macOS at [60] fps with
https://github.com/dngur521/wine-dxgi-framelimiter. Read its README first. Then:
1. Find my Wine prefix and how I launch the game (check $WINEPREFIX / ~/.wine, my
   launcher apps or scripts, and the running processes with `ps -axo comm`).
   Confirm system32\dxgi.dll in that prefix is DXMT's. If it isn't, stop and tell me.
2. Find the game's exe name: ask me to start the game, then look for *.exe processes.
3. Clone the repo and run ./setup.sh (with --prefix if needed). Show me the output.
4. Add `export FL_FPS=[60]` and `export FL_EXE=<the exe>` to my launch script before
   the wine command. Back up the script first. If it has
   DXMT_CONFIG="d3d11.preferredMaxFrameRate=...", comment it out and explain why.
5. Tell me to quit Steam completely, relaunch, and set the in-game frame rate to
   max/unlimited with VSync off.
6. Verify: relaunch once with FL_LOG set and check the log shows "hooked swapchain"
   and fps≈[60]. Then tell me how to turn the log off again.
Don't change anything else in my Wine prefix, and ask before anything risky.
```

If you only have a chat assistant, paste the same text plus the output of `./setup.sh status` and your launch script, and ask it to tell you the exact steps.

## Settings (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `FL_FPS` | *unset* | Frame cap. Unset/`0` = proxy does nothing |
| `FL_EXE` | *any* | Comma-separated exe names to limit (case-insensitive), e.g. `Game.exe,GameLauncher.exe`. Unset or `*` = every DXGI program started with `FL_FPS` |
| `FL_LOG` | *unset* | Windows path of a log file, e.g. `Z:\Users\you\fl.log`. Logs hook installation and the measured fps every 5 s. (`flwine --log FILE` takes a macOS path) |
| `FL_SYNC` | `-1` | `-1` keep the game's SyncInterval; `0` force VSync off; `1`–`4` force it |
| `FL_SPIN_US` | `0` | Busy-wait the last N µs before each deadline. Measured no accuracy gain on Apple Silicon, so off |
| `FL_REAL_DXGI` | `<system32>\dxgi_orig.dll` | Path of the real dxgi |

## Check that it works

```sh
./flwine --fps 60 --exe Game.exe --log ~/fl.log "C:\...\Steam.exe"
tail -f ~/fl.log
```

```
[21:19:22.047 pid=2572] active: fps=60 spin_us=0 sync=-1
[21:19:22.398 pid=2572] hooked factory vtbl=... factory2=1
[21:19:22.428 pid=2572] hooked swapchain vtbl=... present1=yes
[21:19:27.430 pid=2572] fps=59.98 (target 60)
```

No `active:` line → `FL_FPS` didn't reach the game (Steam was already running, or `FL_EXE` doesn't match).
`active:` but no `hooked swapchain` → the game doesn't create its swapchain through DXGI (not D3D10/11/12, or not DXMT).

There is also a test program: `./build.sh && cd build && ../flwine --fps 60 --exe fltest.exe fltest.exe 5` prints the measured fps and frame-time jitter.

Measured with the test program (DXMT, Apple M5, Wine 11):

| | fps | frame time |
|---|---|---|
| no cap | ~740 | 1.3 ms |
| `FL_FPS=60` | 59.96–60.00 | 16.67 ms (σ ≈ 1.5 ms) |
| `FL_FPS=120` | 119.98 | 8.34 ms (σ ≈ 0.9 ms) |

## Uninstall

```sh
./setup.sh uninstall   # puts dxgi_orig.dll back as dxgi.dll
./setup.sh status
```

Updating or reinstalling DXMT overwrites `system32\dxgi.dll`; just run `./setup.sh` again afterwards.

## Compatibility

- **DXMT**: tested (D3D11).
- **DXVK**: not needed — use DXVK's own `dxgi.maxFrameRate`, which already works this way.
- **Wine's builtin wined3d dxgi**: does **not** work (its d3d11 needs private dxgi exports); `setup.sh` refuses unless `--force`.
- **GPTK / D3DMetal / CrossOver**: untested.
- **32-bit games**: not supported (the proxy is 64-bit only).

## ⚠️ Anti-cheat

This replaces a Windows DLL inside the Wine prefix. It doesn't touch the game's own files, but anti-cheat/anti-tamper software may still notice. Don't use it in competitive online games. If a game refuses to start, run `./setup.sh uninstall`. Use at your own risk.

## Building

```sh
brew install mingw-w64
./build.sh        # → build/dxgi.dll, build/fltest.exe
```

## Credits

- [DXMT](https://github.com/3Shain/dxmt) by 3Shain — the Metal-based D3D11/D3D10 translation layer this sits in front of.
- [Aatricks/FrameLimiter](https://github.com/Aatricks/FrameLimiter) — a native macOS Metal frame limiter whose deadline-pacing idea inspired this one (no code shared).
- Proxy-DLL + `Present` hooking is the classic technique of ReShade, Special K and RTSS.

## License

MIT
