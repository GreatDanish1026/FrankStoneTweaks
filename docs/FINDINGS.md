# Findings: The Casting of Frank Stone

Steam app 2223840, build 15770319. Exe `SMG037UE5\Binaries\Win64\CastingFrankStone-Win64-Shipping.exe`,
PE timestamp `0x66ed4485`, Unreal Engine **5.1.1** (crash context: `5.1.1-0+++UE5+Release-5.1`),
DirectX 12, no packer or DRM sections. User settings and logs: `%LOCALAPPDATA%\TheCastingofFrankStone\Saved\`.

## The linker map

The game ships `CastingFrankStone-Win64-Shipping.map` (333 MB, 1.7 million symbols, same
timestamp as the exe), so every function and global has its name and address. Two tools read it:

- `tools/mapsyms.py "<regex>" [n slots]`: symbols by name; with `n`, the vtable slots of the first n
  matches. `@<address>` names the symbol at an address.
- `tools/fnrefs.py <symbol regex>`: the UTF-16 strings a function loads (cvar names), its calls and
  the SetBy priorities it passes in `r8d`. Needs `llvm-objdump` on PATH.

The linker folds identical functions, so a vtable slot can carry an unrelated name.

## Game config

`pakchunk0-Windows.pak` has an unencrypted index; the config inis extract with
`E:\UntilDawnTweaks\tools\pak.py` and an Oodle DLL (`OODLE_DLL`; Call of Duty's
`oo2core_8_win64.dll` decodes it). Relevant `DefaultEngine.ini` values: Lumen GI and reflections,
`r.Lumen.HardwareRayTracing=True`, `r.RayTracing.Shadows=False`, `r.RayTracing.Culling=3` with
radius 5000, virtual shadow maps on, `r.AllowHDR=1`.

`DefaultScalability.ini` has `@Cine` tiers for every group. The menu's Ultra is level 3 for
global illumination and reflections (the user's `GameUserSettings.ini`: `sg.GlobalIlluminationQuality=3`,
`sg.ReflectionQuality=3`; shadows and textures are already 4). What Cine changes over 3:

| group | cvar | 3 | Cine |
|---|---|---|---|
| GI | `r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget` | 300 | 1000 |
| GI | `r.Lumen.ScreenProbeGather.DownsampleFactor` | 16 | 8 |
| GI | `r.Lumen.ScreenProbeGather.TracingOctahedronResolution` | 8 | 16 |
| GI | `r.Lumen.ScreenProbeGather.FullResolutionJitterWidth` | 1 | 0.5 |
| GI | `r.Lumen.TranslucencyVolume.TracingOctahedronResolution` | 3 | 4 |
| GI | `r.Lumen.TranslucencyVolume.RadianceCache.ProbeResolution` | 8 | 16 |
| GI | `r.Lumen.TranslucencyVolume.RadianceCache.NumProbesToTraceBudget` | 200 | 1000 |
| Reflections | `r.Lumen.HardwareRayTracing.LightingMode` | 0 (surface cache) | 1 (hit lighting) |
| Reflections | `r.Lumen.TranslucencyReflections.FrontLayer.Enable` | 0 | 1 |

The engine's `BaseScalability.ini` Cine sections match these; the game adds the lighting mode.

## Menu settings → cvars

`UGameSettingsSMG037` (`/Script/SMG037Runtime.GameSettingsSMG037` in GameUserSettings.ini):

- `SetRayTracingEnabled` sets `r.Lumen.HardwareRayTracing` at priority 0x09 (SetByCode).
- `SetGlobalIlluminationQualitySMG`, `SetReflectionsQuality`, `SetShadowQualitySMG`, ... go through
  `SetQualityCvar<int>(name, level)` (0x141f8a860), also at 0x09. The names come from a table
  filled at startup (globals near 0x1487fa600), presumably the `sg.*` groups.
- `SetAllSettings`, `SetHDRMidLuminance`, `SetHDRMinLuminance` and `SetHDRUILuminance` set
  `r.HDR.Display.MidLuminance`, `.MinLuminanceLog10` and `r.HDR.UI.Luminance` (the menu's
  HDR sliders). Nothing in the game sets the peak.

### Why the add-on doesn't set `sg.*`

`sg.ReflectionQuality` and friends apply their ini section from a change callback inside `Set`
(`Scalability::OnChangeReflectionQuality` → `SetGroupQualityLevel`). Set from ReShade's present
thread, that would run the whole section on the wrong thread, and none of those inner sets would
reach the renderer (see below). The add-on sets the Cine values one by one instead.

## Console variables (UE 5.1)

From `??_7FConsoleManager@@6B@` and `??_7?$FConsoleVariable@H@@6B@`:

- `IConsoleManager`: `FindConsoleVariable` +0xb8, `FindConsoleObject` +0xc0,
  `CallAllConsoleVariableSinks` +0x50. `bCallAllConsoleVariableSinks` is at +0xe0.
- `IConsoleVariable`: `Set(const TCHAR*, EConsoleVariableFlags)` +0x80 (no FName tag in 5.1),
  `GetBool` +0x88, `GetInt` +0x90, `GetFloat` +0x98. IsVariable* slots found from their code
  (expected +0x30), AsVariable* follow them. Flags at +0x18 (top byte = priority).
- Priorities: the menu's code sets at 0x09; `ProcessUserConsoleInput` (the console) at 0x0a.
- `FConsoleVariable<int>::Set` (0x142adeb10): `CanChange` refuses a lower priority than the last
  one; the value is stored; `OnCVarChange` (0x142aca910) copies it to the renderer's copy **only
  on the game thread** and sets the sink flag unless SetBy has bit 0x10000; then the flags get
  the new priority and the change delegate (`+0x20`, count at `+0x28`) is broadcast.

So every `Set` has to run on the game thread. Since 0.1.1 the add-on gives the console manager a
copy of its vtable whose `CallAllConsoleVariableSinks` slot (+0x50) runs the add-on's work first;
`FEngineLoop::Tick` (0x140c06d6c) calls that slot once per frame on the game thread. Values then
reach the renderer as render commands, between frames, and change callbacks (the `sg.*`
groups) run where they expect to. It sets at console priority 0x0a (the menu's 0x09 sets are
then refused, so menu changes can't undo a held value); letting go sets the game's value back
and restores its priority byte.

### 0.1.0 crash and hang (2026-10-26)

0.1.0 set values from ReShade's present thread and copied them to the renderer's copy itself
(the Until Dawn method), mid-frame. Two runs with Cinematic GI and reflections and RT shadows:

- Run 1 crashed at 18:40:03 (crash report `UECC-Windows-79FA5CB2...`): `Streamline/DLSSG present
  failed ... DXGI_ERROR_INVALID_CALL` (CrashType GPUCrash); the RHI thread was in
  `FD3D12Viewport::PresentChecked`. `tools/dumpstacks.py` lists every thread of the minidump.
- Run 2 froze right after `r.RayTracing.Culling=0` was set; no crash report, killed by hand.

Lumen's probe counts and resolutions size GPU resources; changing them mid-frame can leave one
pass using the old size and the next the new, which can fault the GPU. Hence the move to the
game thread. `r.RayTracing.Culling=0` puts every object of the streamed level (foliage
included) into the ray tracing scene, enough on its own for very long frames. Game's mode is 3 (distance OR angle, both sides).

0.1.1 (game-thread sets) ran fine with Cinematic GI and reflections, RT shadows and a 3700-nit
peak, but setting `r.RayTracing.Culling=1` locked the game up as well, so 0.1.2 has no culling
option. Turning RT shadows on stalls and stutters for some seconds first (building the RT shadow
pipeline), then works.

The sink's static at 0x148da2164 is its copy of `r.HDR.Display.MaxLuminance`, stored after a
rebuild (0 until the first one), not the nits argument.

## HDR

The swap chain is `R10G10B10A2` with `SetColorSpace1(12)` (HDR10 PQ). Same mechanism as Until
Dawn: `HDRSettingChangedSinkCallback` (0x144ed0560) rebuilds the tone curve with
`HDRConfigureCVars(hdr, nits, false)` when the HDR state, min/mid/max luminance or the scene
colour multiplier changes; `nits` = `GEngine->GameUserSettings->GetCurrentHDRDisplayNits()`, or
1000 when that is 0.

- `GEngine` 0x148da0c30; `UEngine::GameUserSettings` +0x2b0 (Until Dawn: +0x2a0).
- `GetCurrentHDRDisplayNits` (0x14497a760): `bUseHDRDisplayOutput` (+0x10c) ? `HDRDisplayOutputNits`
  (+0x110) : 0. The user's settings have the flag off, nits 1000, so the curve is always 1000 nits.
- The sink's cached peak: `GHDRMaxLuminnance` at 0x148da2164.

The add-on writes the peak and sets the flag while HDR output is on, and holds
`r.HDR.Display.MaxLuminance` at the peak so the sink runs.

## Ray traced shadows

`r.RayTracing.Shadows` is off in DefaultEngine.ini. `FOcclusionRGS` (the RT shadow ray-gen
shader) is in the cooked `GlobalShaderCache-PCD3D_SM6.bin`, so it should run. Per-light-type
switches (`r.RayTracing.Shadows.Lights.*`) and `SamplesPerPixel` exist. Not yet confirmed in game.

## Other

- A stable PSO cache ships (`Content/PipelineCaches/Windows/SMG037UE5_PCD3D_SM6.stable.upipelinecache`);
  `r.PSOPrecaching` exists (experimental in 5.1). Players still report stutter.
- Old crash (May 2025, RTX 4070 Ti SUPER): DXGI device hung (887A0006).

## GPU hang with 0.1.2 (2026-10-26 19:02)

`CurrentQueue.Device->GetDevice()->GetDeviceRemovedReason() failed ... 887A0006` (DXGI_ERROR_DEVICE_HUNG),
67 s after start, with Cinematic GI, Cinematic reflections, RT shadows and a 3700-nit peak all
applied at the title screen (crash `UECC-Windows-F002856C...`). The same error is in the user's
May 2025 crash report with no add-on, so the game can hang on its own; the question is which
option makes it likely.

No way to see the hung pass: logging is compiled out (`LogD3D12RHI` is an `FNoLoggingCategory`),
and the shipped Aftermath dump writer (`AftermathGpuCrashDumpCallback`) only writes Vulkan dumps.
Bisecting instead: next run without RT shadows (never enabled by the game, so untested on its
content), then without the Cinematic reflections (hit lighting).
