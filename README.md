# FrankStoneTweaks 0.1.2

Graphics add-on for **The Casting of Frank Stone** (PC, Steam), for ReShade with add-on
support. Raises the game's ray tracing past what its menu offers and sets the HDR peak
brightness, live, from an in-game overlay.

- **Ray tracing**
  - *Global illumination: Cinematic*: the game's own unused tier above Ultra. It places 4x as
    many lighting probes, traces twice the directions per probe and lights fog more finely.
  - *Reflections: Cinematic*: the game's unused tier above Ultra. Reflections are lit
    properly where the ray hits, and glass and water get ray traced reflections too.
  - *Ray traced shadows*: the engine supports them, but the game ships with them off. The
    first time they come on in a session the game builds their shaders: several seconds of
    stutter.
- **HDR**: set the peak brightness. The game always builds its tone curve for 1000 nits. Its
  own HDR sliders (brightness, black level, UI) still apply.
- **F8** switches every tweak off and on, to compare.

Every option starts at the game's own value, and its tooltip explains what it changes.

## Requirements

- The Casting of Frank Stone for PC (tested on Steam build 15770319). On another build, the
  HDR peak option is disabled and the overlay says so.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `CastingFrankStone-Win64-Shipping.exe` with DirectX 10/11/12.
- Reflections need Ray Tracing on in the game's graphics menu.

## Install

1. Install ReShade with add-on support into `...\The Casting of Frank Stone\SMG037UE5\Binaries\Win64`.
2. Copy `FrankStoneTweaks.addon64` into the same folder.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under FrankStoneTweaks.

Settings are saved in `ReShade.ini` under `[FrankStoneTweaks]` and come back at every start.
No config files are written; deleting the `.addon64` file removes the mod.

## Building

`.\build.ps1` builds `bin\FrankStoneTweaks.addon64` (needs Visual Studio 2022 with the C++
tools). `.\build.ps1 -Install` also copies it into the game folder (close the game first).
See `docs/FINDINGS.md` for how the game's settings were found.
