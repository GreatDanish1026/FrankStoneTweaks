# FrankStoneTweaks 0.7.1

Graphics add-on for **The Casting of Frank Stone** (PC, Steam), for ReShade with add-on
support. Raises the game's ray tracing past what its menu offers and sets the HDR peak
brightness, live, from an in-game overlay.

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
- **Detail** (carried over from our other Unreal add-ons, past what the menu's Ultra reaches):
  texture memory up to 8 GB (the menu gives 3 GB), 16x anisotropic filtering (the game uses 8x),
  view distance 2x or the game's unused Cinematic 10x, finer volumetric fog, and the game's
  unused Cinematic tiers for lamp soft shadows, strand hair, and the sky and haze.
- **Camera**: switches for effects the menu can't turn off: depth of field (the menu only has
  Normal and Ultra), chromatic aberration, vignette, the game's lens flares (or just their
  anamorphic streaks), bloom (or full-resolution bloom), fog glow, and the always-on
  sharpening. Motion blur and film grain are left to the menu.
- **Stutter** (experimental): *Shader precaching* compiles the shaders each object needs while
  its level loads, instead of the first time it comes on screen; *skip unready draws* also
  lets an object pop in a frame late rather than hitch. A **hitch meter** counts frames over
  50, 100 and 250 ms (and logs each big one) so you can compare with it off and on, and the
  tab shows whether the game's startup shader compile has finished.
- **HDR**: set the peak brightness. The game always builds its tone curve for 1000 nits. Its
  own HDR sliders (brightness, black level, UI) still apply.
- **Quality presets**: *Ultra+* (the cheaper upgrades: 6 GB textures, 16x filtering, 2x view
  distance, finer fog, Cinematic lamp shadows, hair and sky) and *Cinematic* (everything on the
  Ray tracing and Detail tabs at its highest), plus *Cinematic-lite* (Cinematic with High global
  illumination instead of Cinematic). Camera effects and HDR are left alone.
  Measured on an RTX 5080 at 4K with DLSS Quality and Frame Generation in one scene: Game 146 fps,
  Ultra+ 135 fps (about 8% slower), Cinematic-lite 129 fps (about 14% slower), Cinematic 87 fps
  (about 70% slower).
  **Measure preset costs** runs each preset for a few seconds and shows its frame rate on your PC.
  Each Ray tracing and Detail option also has a **Measure** button that does the same for its
  choices, with everything else left as it is.
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
