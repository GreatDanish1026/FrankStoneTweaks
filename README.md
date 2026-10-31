# FrankStoneTweaks 1.0.0

Graphics add-on for **The Casting of Frank Stone** (PC, Steam), for ReShade with add-on
support. Goes past the game's Ultra settings with its own unused Cinematic tiers and extra ray
tracing, switches off camera effects the menu can't, sets the HDR peak brightness, and helps
with shader stutter, all live from an in-game overlay.

- **Ray tracing**
  - *Global illumination*: *High* raises the lighting cache's update budget and uses the
    Cinematic lighting for fog, glass and hair, at little cost. *Very high* adds 4x as many
    rays per lighting probe at the game's probe spacing. *Cinematic* is the game's own unused
    tier above Ultra: 4x as many probes on top of that. It's much softer and steadier, but by
    far the most expensive option here. Measured in one scene: High +2%, Very high +10%,
    Cinematic +37% frame time.
  - *Reflections: Cinematic*: the game's unused tier above Ultra. Reflections are lit
    properly where the ray hits, and glass and water get ray traced reflections too.
  - *Reflections on rougher surfaces*: ray traced reflections up to roughness 0.6, 0.8 or all
    surfaces (the game stops at 0.4; rougher ones get a blurry approximation).
  - *Reflection screen traces: Off*: reflections come only from ray tracing, so they no
    longer fade at the screen edges or vanish behind foreground objects.
- **Detail** (past what the menu's Ultra reaches): texture memory up to 8 GB (the menu gives
  3 GB), 16x anisotropic filtering (the game uses 8x), view distance 2x or the game's unused
  Cinematic 10x, finer volumetric fog, and the game's unused Cinematic tiers for lamp soft
  shadows, strand hair, and the sky and haze.
- **Camera**: switches for effects the menu can't turn off: depth of field (the menu only has
  Normal and Ultra), chromatic aberration, vignette, the game's lens flares (or just their
  anamorphic streaks), bloom (or full-resolution bloom), fog glow, and the always-on
  sharpening. Motion blur and film grain are left to the menu.
- **Stutter** (experimental): *Shader precaching* compiles the shaders each object needs while
  its level loads, instead of the first time it comes on screen; *skip unready draws* also
  lets an object pop in a frame late rather than hitch. Takes effect for areas loaded after it
  is turned on. A **hitch meter** counts frames over 50, 100 and 250 ms (and logs each big
  one), and the tab shows whether the game's startup shader compile has finished. The large
  hitches left are at scene changes, where the game loads and rebuilds the lighting for a new
  view.
- **HDR**: set the peak brightness. The game always builds its tone curve for 1000 nits. Its
  own HDR sliders (brightness, black level, UI) still apply.
- **Quality presets** set the Ray tracing and Detail tabs together (Camera, Stutter and HDR are
  left alone):
  - *Ultra+*: the cheaper upgrades: 6 GB textures, 16x filtering, 2x view distance, finer fog,
    Cinematic lamp shadows, hair and sky. Ray tracing stays as the menu sets it.
  - *Cinematic-lite*: Very high global illumination, Cinematic reflections up to roughness
    0.6 without screen traces, 8 GB textures, 10x view distance, the finest fog, and the
    Cinematic lamp shadows, hair and sky.
  - *Cinematic*: everything at its highest, including Cinematic global illumination and
    reflections on every surface.

  Measured on an RTX 5080 at 4K with DLSS Quality and Frame Generation (Game 112 fps in that
  scene): Ultra+ about 5% slower (8% in a lighter scene), Cinematic-lite about 30%, Cinematic
  about 75%. **Measure preset costs** times each preset on your PC, and every Ray tracing and
  Detail option has a **Measure** button that times its choices.
- **F8** switches every tweak off and on, to compare.

Every option starts at the game's own value, and its tooltip explains what it changes.

## Not included

- **Ray traced shadows**: the engine has them, but the game ships with them off, and turning
  them on hung the GPU within minutes (also without hair tracing). Left out.
- **Ray tracing range**: changing which objects are in the ray tracing scene while the game
  runs locks it up. Left out.
- **RTX Dynamic Vibrance**: the game never loads it (it lacks the engine plugin that does), so
  it can't be switched on from an add-on. The NVIDIA app's own RTX Dynamic Vibrance filter
  works instead.

## Requirements

- The Casting of Frank Stone for PC (tested on Steam build 15770319). On another build, the
  HDR peak option and the shader compile status are disabled and the overlay says so.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `CastingFrankStone-Win64-Shipping.exe` with DirectX 10/11/12.
- Reflections need Ray Tracing on in the game's graphics menu.

## Install

1. Install ReShade with add-on support into `...\The Casting of Frank Stone\SMG037UE5\Binaries\Win64`.
2. Copy `FrankStoneTweaks.addon64` into the same folder.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under FrankStoneTweaks.

Settings are saved in `ReShade.ini` under `[FrankStoneTweaks]` and come back at every start.
The add-on writes no config files of its own, and deleting the `.addon64` file removes it. One
exception: while a custom HDR peak is set, the game may save that peak to its own
`GameUserSettings.ini` (`HDRDisplayOutputNits`); setting the peak back to the game's restores
it. `FrankStoneTweaks.log` next to the exe records what the add-on did.

## Building

`.\build.ps1` builds `bin\FrankStoneTweaks.addon64` (needs Visual Studio 2022 with the C++
tools). `.\build.ps1 -Install` also copies it into the game folder (close the game first).
See `docs/FINDINGS.md` for how the game's settings were found, and `tools/` for the scripts
that read the game's shipped linker map and crash dumps.
