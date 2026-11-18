# Battleship — PS Vita homebrew (VitaSDK + vita2d)

A dark naval-radar Battleship made for the PCH-1000 OLED (960×544, 60 FPS). You can play against a Hunt & Target CPU, or against another Vita over Ad-Hoc with no router. Every sound is synthesised in code, so the game needs no audio files.

- **Title ID / Ad-Hoc product ID:** `BTLSHP001`
- **Fleet:** 1×4, 2×3, 3×2 and 4×1 ships on a 10×10 grid. Ships may not touch, even diagonally.
- **Rules:** A hit gives you another shot. When a ship sinks, its whole outline is shown and the water around it is marked as misses.

## Project structure

```
battleship-vita/
├── CMakeLists.txt              VitaSDK build → Battleship.vpk
├── include/
│   ├── app.h                   shared App state (state machine data)
│   ├── game_logic.h            board, ships, radar, RNG
│   ├── ai.h                    Hunt & Target AI
│   ├── adhoc_net.h             Ad-Hoc API + 8-byte wire protocol
│   ├── audio.h                 SFX ids
│   └── render.h                palette, layout, input, effects
├── src/
│   ├── main.c                  main loop + state machine (Title, Mode, Lobby, Setup, Battle, Game Over)
│   ├── game_logic.c            placement validation, combat rules
│   ├── ai.c                    probability-density hunt + parity, axis-locking target mode
│   ├── adhoc_net.c             SceNetAdhocMatching discovery + PTP sockets on a worker thread
│   ├── audio.c                 PCM synthesiser + sceAudioOut mixer thread (12 voices)
│   └── render.c                vita2d UI, radar sweep, shockwaves, shake, fades, touch/buttons
├── sce_sys/
│   ├── icon0.png  pic0.png
│   └── livearea/contents/ bg.png startup.png template.xml
├── tools/gen_livearea.py       regenerates the LiveArea art (Pillow)
└── tests/
    ├── test_logic.c            rules + 5,000 AI games (host-side)
    ├── dump_sfx.c              renders all synthesised SFX to .wav
    └── host_sim/               runs the REAL game code on Linux with stubbed Vita APIs,
                                including a two-process Ad-Hoc match
```

`param.sfo` is not stored in the repo. `vita_create_vpk` generates it at build time from `VITA_TITLEID`, `VITA_APP_NAME` and `VITA_VERSION`.

## Building the VPK

### Option A: Debian/Ubuntu (VM or bare metal)

```bash
sudo apt install make git cmake python3 curl bzip2 xz-utils
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH        # add both lines to ~/.bashrc

git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh                # toolchain
./install-all.sh                      # libraries: vita2d, freetype, libpng, libjpeg, zlib, ...
cd ..

cd battleship-vita
cmake -S . -B build
cmake --build build -j$(nproc)
# -> build/Battleship.vpk
```

### Option B: Docker (official image)

```bash
docker run --rm -v "$PWD":/src -w /src vitasdk/vitasdk \
  sh -c "cmake -S . -B build && cmake --build build -j"
```

### Install on the Vita

Copy `build/Battleship.vpk` to `ux0:` using VitaShell (USB or FTP). Then press ✕ on it and choose Install. With VitaShell's FTP server running, `cmake -S . -B build -DVITA_IP=192.168.x.x && cmake --build build --target send` uploads the VPK to `ux0:/data/`.

## Controls

| Input | Setup | Battle | Menus |
|---|---|---|---|
| D-Pad / left stick | move ship (auto-repeat; a full stick tilt moves faster) | move crosshair | navigate |
| ✕ | place ship / READY once all are placed | fire | select |
| ○ | undo last ship (back to menu if none are placed) | surrender (asks first) | back |
| △, L, R | rotate | – | – |
| □ | randomise the whole fleet | – | – |
| START | READY | – | – |
| Touch | tap a cell to place; ROTATE / RANDOM / UNDO / READY buttons | tap a radar cell to fire | tap items |

## Ad-Hoc multiplayer

1. Turn Wi-Fi on and Airplane mode off on both consoles. No router is needed.
2. On Vita A, choose **HOST AD-HOC GAME**. It advertises your system nickname.
3. On Vita B, choose **JOIN AD-HOC GAME** and pick the host from the list.
4. Both players place their fleets and press READY. The host fires first.
5. After the game, both players press REMATCH, or either can return to the menu.

**How the network code works**

| Layer | What it does |
|---|---|
| `sceNetAdhocctlInit` | uses product ID `BTLSHP001`, so only consoles running this game see each other |
| Matching (PARENT/CHILD) | the host's hello-opt carries `{magic "BSHP", version, MAC, nickname}`; the joiner sends the same block back in `SelectTarget` |
| PTP | the host listens on port `0x5342` and the joiner connects to the host MAC from the hello. All traffic runs on a worker thread, so the 60 FPS loop never blocks |
| Packets (8 bytes) | `HELLO`, `READY`, `FIRE(x,y)`, `RESULT(x,y,status, sunk x,y,len,horiz)`, `REMATCH`, `QUIT`, `PING` (keep-alive once per second) |
| Disconnects | PTP errors, a matching BYE/LEAVE, or 8 s without any packet show an alert and return you to the main menu. If the opponent quits after the result is decided, the result screen stays up and only REMATCH is disabled |

### ⚠ Not yet tested on real hardware

The project compiles cleanly against the current `vita-headers` and `libvita2d` for ARMv7. The real game code (main.c, adhoc_net.c and the rest) was also run on Linux with the Vita APIs stubbed out. That covered single-player, a two-process Ad-Hoc match with rematches, a quit after the game is decided, and a console powering off mid-battle. It did **not** run on a Vita. These Ad-Hoc details come from the PSP SDK and the headers, and only a real console can confirm them:

- **Matching event codes.** They are assumed to match the PSP: 1 = HELLO, 2 = REQUEST, 7 = ESTABLISHED, 10 = BYE, and so on (`MEV_*` in `adhoc_net.c`).
- **The Ad-Hoc link.** The code assumes the system creates or joins the Ad-Hoc link itself once `sceNetAdhocctlInit` has run with a product ID. The public NID database exports no create/connect call.

If something fails, the error dialog shows the failing call and its `0x8041xxxx` code. That code points to the exact step.

## Single-player AI

- **Hunt:** For every unknown cell, it counts how many legal placements of the remaining ships would cover that cell. It only considers cells on a checkerboard lattice spaced by the shortest surviving ship.
- **Target:** After a hit, it probes the orthogonal neighbours, best-scoring first. After a second hit it locks the axis and works along both ends until the ship sinks. Because ships can't touch, it also marks the diagonal neighbours of every hit as water.
- **Thinking delay:** 0.6 s before each shot, then about 0.45 s while the shell flies.
- **Benchmark** (`tests/test_logic.c`, 5,000 games): it sinks the whole fleet in **55 shots on average** (best 32, worst 77). Random firing takes about 96.

## Tests (no Vita needed)

```bash
# rules + AI
cc -O2 -Iinclude tests/test_logic.c src/game_logic.c src/ai.c -o test_logic && ./test_logic

# listen to the synthesised SFX
cc -O2 -DAUDIO_HOST_DUMP -Iinclude tests/dump_sfx.c src/audio.c -lm -o dump_sfx && ./dump_sfx .

# full-game simulation with stubbed Vita APIs (needs vita-headers + libvita2d headers)
VITA_HEADERS=$VITASDK/arm-vita-eabi/include VITA2D_INC=$VITASDK/arm-vita-eabi/include sh tests/host_sim/build.sh
BS_ROLE=single BS_GAMES=3 ./tests/host_sim/bs_sim
BS_ROLE=host BS_GAMES=3 ./tests/host_sim/bs_sim & sleep 0.3; BS_ROLE=join BS_GAMES=3 ./tests/host_sim/bs_sim
```

## Customising

- **Colours:** the `COL_*` values in `include/render.h`.
- **Sounds:** the `synth_*` functions in `src/audio.c`. Use `dump_sfx` to audition changes.
- **Timing:** `AI_THINK_S`, `SHELL_FLIGHT` and `SONAR_EVERY` in `src/main.c`.
- **LiveArea art:** run `python3 tools/gen_livearea.py`. The PNGs are written 8-bit indexed, which the LiveArea requires.
