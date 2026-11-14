# Anti-Lag (FH3)

Anti-lag for turbo cars in **Forza Horizon 3** (the Xbox One build on PC, through the Azimuth launcher), a mod for the
**[FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform)**. Inspired by erikzensounds' FH2 Anti-Lag; written from scratch for FH3.

> Version **1.0.0**. Needs the FH3 Mod Platform **1.4.2** or newer.

## What it does

- **An upgrade you buy.** In the game's own turbo list (**Garage > Upgrades > Engine > Turbo / Twin Turbo**): **Race Turbo w/ Anti-Lag**, after Street, Sport and Race. It is the Race turbo plus a price (2 000 CR by default).
- **The boost stays on lift-off.** With the upgrade, lifting the throttle above the cutoff RPM keeps the turbo spooled: the boost bleeds away slowly instead of at once, so there is no lag when you get back on the throttle.
- **Exhaust pops and flames.** While anti-lag is working the car crackles and pops, with flames from the pipes. The sounds are played by the game itself (same position, muffling and reverb); **Mods > Anti-Lag > Backfire sounds** switches between the game's own backfire samples and the mod's (FH6) ones.
- **Tune it per car.** The car's **Tuning > Engine** tab gets four rows (only on cars with the upgrade): **Pressure decay (ms)**, **Boost retention (%)**, **Cutoff RPM** and **Pop interval (ms)**. The values are saved per garage car in `fh3-mods\AntiLag.ini`.

## Requirements

| What | Version |
|---|---|
| Forza Horizon 3 on PC with the **Azimuth** launcher | Xbox One build 1.0.125.0 (the one the platform targets) |
| **[FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform)** | **1.4.2 or newer** |

Offline only: the mod is switched off while online.

## Install

Put `AntiLag.dll` into the `fh3-mods` folder next to the game's platform files and start the game. Buy the turbo on a car in the garage (the car needs a turbo to start with).

## Settings

`fh3-mods\AntiLag.ini` is created on the first start.

`[AntiLag]`:

| Key | Default | What |
|---|---|---|
| `ExtraPrice` | 2000 | What the anti-lag turbo costs more than the Race turbo |
| `PopSound` | 1 | 1 = the mod's backfire samples play instead of the game's; 0 = the game's own (also on the Mods page) |
| `Flames` | 1 | Flames from the exhaust on a pop |
| `Native` | 1 | 1 = the game plays the pops itself; 0 = the old way, the mod's own voice |
| `Sound`, `SoundVolume`, `Reverb`, `FlipPan`, `FMOD` | Auto, 55, 8, 0, 1 | Sound details of the mod's own voice (`Native=0`) |
| `Log` | 0 | 1 writes the mod's ordinary lines to `platform.log`; problems are always logged |

`[Defaults]`: `Decay` (6000), `Retention` (85), `Cutoff` (1800), `Pop` (140): the values of a car whose rows were never touched. `[Car<id>]` sections hold the per-car values.

## Build

Needs CMake and a MinGW-w64 (llvm-mingw) toolchain. Put the FH3 Mod Platform checkout at `./platform` (or pass `-DFH3_PLATFORM_DIR=...`) and build:

    cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<your mingw toolchain>
    cmake --build build

`sounds.pak` (the samples) is built into the DLL; `tools/pack_sounds.py` makes it.

Developer tool: **F8** records the player car's physics objects for about 14 seconds to `fh3-mods\antilag-probe.bin`; it is how the turbo state was found.

Releasing: [RELEASING.md](RELEASING.md). Changes: [CHANGELOG.md](CHANGELOG.md).

License: MIT.
