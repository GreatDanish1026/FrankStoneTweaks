// FrankStoneTweaks: a ReShade add-on for The Casting of Frank Stone (UE 5.1.1, DirectX 12) that
// raises the game's ray tracing past its menu and sets the HDR peak brightness, live, from an
// in-game overlay.
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, UE 5.1 layout read from the linker map the game ships). Values are set on the game
// thread, from the console manager's once-per-frame CallAllConsoleVariableSinks (GameTick), so
// they reach the renderer between frames like the console's own. They are set at console
// priority, above the game's own menu code, so they stay until the option goes back to Game;
// then the game's value and priority are put back. A check every second puts back anything
// that changed anyway. F8 switches all of it off and on for comparison.
//
// Threads: ReShade's present thread draws the overlay and only changes options; the game
// thread does all setting. Both take g_state; the game thread only tries it and skips a frame
// if the overlay holds it, so it never waits on the present thread.
//
// No Engine.ini is written; nothing is left behind when the add-on is removed (see the HDR
// section for the one exception while the game runs).
// Writes FrankStoneTweaks.log next to the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = FST_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Cinematic ray tracing, extra detail, camera effect switches and HDR peak brightness for " FST_GAME
    ", live from the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring file(path, n);
    file.resize(file.find_last_of(L"\\/") + 1);
    file += L"FrankStoneTweaks.log";
    g_log = _wfopen(file.c_str(), L"w");
    Log(FST_PRODUCT " " FST_VERSION " loaded");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. "Cinematic" values are the game's own [GlobalIlluminationQuality@Cine] and
// [ReflectionQuality@Cine] sections (DefaultScalability.ini), which the menu never uses: its
// Ultra is level 3. Only the keys that differ from level 3 are listed (docs/FINDINGS.md).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

std::vector<Option> g_options = {
    {"Ray tracing", "Global illumination", "CineGi", "Global illumination",
     "Lumen's bounce lighting.\n"
     "High: over 3x the lighting cache's update budget (bounce light keeps up when the\n"
     "camera moves, with less lag and pop-in) and the Cinematic lighting in fog and on\n"
     "glass and hair. Keeps the game's probe spacing, so it costs little.\n"
     "Cinematic: the game's own unused tier above Ultra. High plus lighting probes every\n"
     "8 pixels instead of 16 and twice the directions per probe, about 16x the probe\n"
     "tracing: softer, more stable indirect light, but in one scene it cost about 60% of\n"
     "the frame rate at 4K. For photo mode or screenshots.\n"
     "Applies with or without the menu's Ray Tracing.",
     L"r.Lumen.ScreenProbeGather.DownsampleFactor",
     {{"Game", 0, {}},
      {"High", 2,
       {{L"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", L"1000"},
        {L"r.Lumen.TranslucencyVolume.TracingOctahedronResolution", L"4"},
        {L"r.Lumen.TranslucencyVolume.RadianceCache.ProbeResolution", L"16"},
        {L"r.Lumen.TranslucencyVolume.RadianceCache.NumProbesToTraceBudget", L"1000"}}},
      {"Cinematic", 1,
       {{L"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", L"1000"},
        {L"r.Lumen.ScreenProbeGather.DownsampleFactor", L"8"},
        {L"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", L"16"},
        {L"r.Lumen.ScreenProbeGather.FullResolutionJitterWidth", L"0.5"},
        {L"r.Lumen.TranslucencyVolume.TracingOctahedronResolution", L"4"},
        {L"r.Lumen.TranslucencyVolume.RadianceCache.ProbeResolution", L"16"},
        {L"r.Lumen.TranslucencyVolume.RadianceCache.NumProbesToTraceBudget", L"1000"}}}}},
    {"Ray tracing", "Reflections", "CineReflections", "Reflections",
     "Cinematic is the game's own unused tier above Ultra. Ray traced reflections are lit\n"
     "properly where the ray hits (the menu's Ultra uses Lumen's rough cached lighting\n"
     "instead), and glass, windows and water get ray traced reflections too (off in the\n"
     "menu's tiers). Needs the menu's Ray Tracing on.",
     L"r.Lumen.HardwareRayTracing.LightingMode",
     {{"Game", 0, {}},
      {"Cinematic", 1,
       {{L"r.Lumen.HardwareRayTracing.LightingMode", L"1"},
        {L"r.Lumen.TranslucencyReflections.FrontLayer.Enable", L"1"}}}}},
    // ------------------------------------------------ detail (from DirectiveTweaks / UntilDawnTweaks)
    // The menu's Ultra textures and shadows are already the game's Cinematic tier (3000 MB
    // pool, 8192 shadow pages); these go past what the menu reaches.
    {"Detail", "Textures", "TexturePool", "Texture memory",
     "Video memory for streamed textures. The menu's Ultra gives 3 GB whatever the card,\n"
     "so textures can go blurry and reload in larger scenes. The RTX 5080 has 16 GB.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"4 GB", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB", 6144, {{L"r.Streaming.PoolSize", L"6144"}}},
      {"8 GB", 8192, {{L"r.Streaming.PoolSize", L"8192"}}}}},
    {"Detail", "Textures", "Anisotropy", "Anisotropic filtering",
     "Sharper floors, roads and walls at shallow angles. The game uses 8x. Textures already\n"
     "loaded keep their filtering until they reload (or the game restarts).",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}, {L"r.VT.MaxAnisotropy", L"16"}}}}},
    {"Detail", "Distance", "ViewDistance", "View distance",
     "How far away objects keep drawing and keep their detailed versions. The game's Ultra\n"
     "is 1x; Cinematic is the game's own unused tier (10x). Costs more in open areas.",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}},
      {"Far (2x)", 2, {{L"r.ViewDistanceScale", L"2"}}},
      {"Cinematic (10x)", 10, {{L"r.ViewDistanceScale", L"10"}}}}},
    {"Detail", "Lighting", "FogCells", "Volumetric fog detail",
     "Size of the fog's light grid in screen pixels: smaller cells give crisper flashlight\n"
     "beams and light shafts and less blocky shadows in fog. The game's Ultra uses 8 px;\n"
     "4 px is about 4x the fog cost.",
     L"r.VolumetricFog.GridPixelSize",
     {{"Game", 0, {}},
      {"Fine (6 px)", 6,
       {{L"r.VolumetricFog.GridPixelSize", L"6"}, {L"r.VolumetricFog.GridSizeZ", L"128"},
        {L"r.VolumetricFog.HistoryMissSupersampleCount", L"16"}}},
      {"Finest (4 px)", 4,
       {{L"r.VolumetricFog.GridPixelSize", L"4"}, {L"r.VolumetricFog.GridSizeZ", L"128"},
        {L"r.VolumetricFog.HistoryMissSupersampleCount", L"16"}}}}},
    {"Detail", "Lighting", "SoftShadows", "Soft shadows from lamps",
     "Shadow-map rays per pixel for lamps, torches and flashlights. The game's Ultra uses\n"
     "8 rays of 5 samples (16 of 8 for the moon and sun); Cinematic gives lamps the same\n"
     "16 of 8, for smoother soft shadow edges.",
     L"r.Shadow.Virtual.SMRT.RayCountLocal",
     {{"Game", 0, {}},
      {"Cinematic", 1,
       {{L"r.Shadow.Virtual.SMRT.RayCountLocal", L"16"}, {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"8"}}}}},
    {"Detail", "Characters and sky", "HairDetail", "Hair detail",
     "The game's unused Cinematic tier for strand hair: 8 coverage samples per pixel\n"
     "instead of 4 (smoother, less aliased hair edges) and finer sky lighting and\n"
     "ambient occlusion on hair.",
     L"r.HairStrands.Visibility.MSAA.SamplePerPixel",
     {{"Game", 0, {}},
      {"Cinematic", 1,
       {{L"r.HairStrands.SkyLighting.IntegrationType", L"0"},
        {L"r.HairStrands.SkyAO.SampleCount", L"8"},
        {L"r.HairStrands.Visibility.MSAA.SamplePerPixel", L"8"}}}}},
    {"Detail", "Characters and sky", "SkyDetail", "Sky and haze detail",
     "The game's unused Cinematic tier for the sky and distance haze: the sky is computed\n"
     "per pixel instead of from a small lookup table, with more samples, and haze over\n"
     "distant objects is computed in finer depth slices. Smoother gradients at dusk and night.",
     L"r.SkyAtmosphere.FastSkyLUT",
     {{"Game", 0, {}},
      {"Cinematic", 1,
       {{L"r.SkyAtmosphere.AerialPerspectiveLUT.FastApplyOnOpaque", L"0"},
        {L"r.SkyAtmosphere.AerialPerspectiveLUT.SampleCountMaxPerSlice", L"8"},
        {L"r.SkyAtmosphere.AerialPerspectiveLUT.DepthResolution", L"32"},
        {L"r.SkyAtmosphere.FastSkyLUT", L"0"},
        {L"r.SkyAtmosphere.FastSkyLUT.SampleCountMax", L"256"},
        {L"r.SkyAtmosphere.SampleCountMin", L"8"},
        {L"r.SkyAtmosphere.SampleCountMax", L"256"},
        {L"r.SkyAtmosphere.TransmittanceLUT.SampleCount", L"30"},
        {L"r.SkyAtmosphere.MultiScatteringLUT.SampleCount", L"20"}}}}},

    // ------------------------------------------------ camera effects the menu can't turn off
    // The menu has switches for motion blur and film grain, so those aren't here. Its depth of
    // field setting only offers Normal and Ultra.
    {"Camera", "Lens", "DepthOfField", "Depth of field",
     "Background and foreground blur in cutscenes and close-ups. The menu only offers\n"
     "Normal and Ultra.",
     L"r.DepthOfFieldQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.DepthOfFieldQuality", L"0"}}}}},
    {"Camera", "Lens", "ChromaticAberration", "Chromatic aberration",
     "Red and blue colour fringes towards the screen edges, imitating a cheap lens.",
     L"r.SceneColorFringeQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.SceneColorFringe.Max", L"0"}}}}},
    {"Camera", "Lens", "Vignette", "Vignette",
     "Darkened screen corners. Off drops the tonemapper to quality 1, which also removes\n"
     "film grain (also a switch in the game's menu).",
     L"r.Tonemapper.Quality", {{"Game", 0, {}}, {"Off", 1, {{L"r.Tonemapper.Quality", L"1"}}}}},
    {"Camera", "Lens", "LensFlares", "Lens flares",
     "The game's own lens flare: ghosts and halos, star-shaped glare and horizontal\n"
     "anamorphic streaks across bright lights. No streaks keeps the rest.",
     L"r.LensFlare.RenderAnamorphicGlare",
     {{"Game", 0, {}},
      {"No streaks", 1, {{L"r.LensFlare.RenderAnamorphicGlare", L"0"}}},
      {"Off", 2,
       {{L"r.LensFlare.RenderFlare", L"0"}, {L"r.LensFlare.RenderGlare", L"0"},
        {L"r.LensFlare.RenderAnamorphicGlare", L"0"}}}}},
    {"Camera", "Light", "Bloom", "Bloom",
     "Glow around bright lights. The game renders it at 71% resolution; Full resolution\n"
     "is its unused Cinematic value. Off removes the glow entirely (lamps and the moon\n"
     "look harsher, especially in HDR).",
     L"r.Bloom.ScreenPercentage",
     {{"Game", 0, {}}, {"Full resolution", 1, {{L"r.Bloom.ScreenPercentage", L"100"}}},
      {"Off", 2, {{L"r.BloomQuality", L"0"}}}}},
    {"Camera", "Light", "FogGlow", "Fog glow",
     "Supermassive's screen-space fog scattering: bright lights bleed a soft glow into\n"
     "fog, which also softens the whole image in foggy scenes.",
     L"r.SSFS", {{"Game", 0, {}}, {"Off", 1, {{L"r.SSFS", L"0"}}}}},
    {"Camera", "Image", "Sharpening", "Sharpening",
     "The game always sharpens the final image (strength 1.5), on top of DLSS or TSR,\n"
     "which can leave halos on edges and crawl on fine detail.",
     L"r.Tonemapper.Sharpen",
     {{"Game", 0, {}}, {"Light (0.5)", 1, {{L"r.Tonemapper.Sharpen", L"0.5"}}},
      {"Off", 2, {{L"r.Tonemapper.Sharpen", L"0"}}}}},

    // No ray traced shadows: r.RayTracing.Shadows=1 hung the GPU within minutes, with all its
    // features, without hair voxel tracing too (0.1.2, 0.1.3). docs/FINDINGS.md.
    // No ray tracing range option: changing r.RayTracing.Culling while the game runs locked it
    // up, with 0 (0.1.0) and with 1 (0.1.1, set on the game thread). docs/FINDINGS.md.
};

// Guards everything the overlay and the game thread share: options, the maps below, the
// lookup cache.
std::recursive_mutex g_state;

// HDR peak brightness in nits (0 = the game's).
int g_hdrPeak = 0;
std::atomic<bool> g_enabled{true};  // F8

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

void LoadSettings() {
    for (Option& o : g_options) {
        reshade::get_config_value(nullptr, FST_PRODUCT, o.key, o.value);
        if (!Chosen(o)) o.value = 0;
    }
    reshade::get_config_value(nullptr, FST_PRODUCT, "HdrPeakNits", g_hdrPeak);
    if (g_hdrPeak && (g_hdrPeak < 400 || g_hdrPeak > 10000)) g_hdrPeak = 0;
}

void SaveSettings() {
    for (const Option& o : g_options) reshade::set_config_value(nullptr, FST_PRODUCT, o.key, o.value);
    reshade::set_config_value(nullptr, FST_PRODUCT, "HdrPeakNits", g_hdrPeak);
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
std::string g_liveError;

std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, uint32_t> g_origPriority;   // ...and the priority it was set at
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)
std::atomic<bool> g_dirty{true};                   // options changed: apply at the next game tick
int g_reapplied = 0;                               // values put back after they changed
std::vector<std::pair<std::wstring, std::wstring>> g_pendingConsole;  // Advanced tab's "Set once"
std::atomic<bool> g_hooked{false};                 // game-thread hook in place
std::atomic<uint64_t> g_ticks{0};                  // game ticks seen by the hook

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

int ReadInt(const wchar_t* name) {
    std::wstring v;
    return g_live.load() == Live::Ready && Read(name, v) ? _wtoi(v.c_str()) : -1;
}

float ReadFloat(const wchar_t* name, float fallback) {
    std::wstring v;
    return g_live.load() == Live::Ready && Read(name, v) ? static_cast<float>(_wtof(v.c_str())) : fallback;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

// How many change callbacks a variable has: its OnChangedCallback (a multicast delegate at
// +0x20, invocation count at +0x28; FConsoleVariableBase, read from Set's code).
int Callbacks(void* var) {
    int32_t n = -1;
    cvars::SafeCopy(&n, static_cast<uint8_t*>(var) + 0x28, 4);
    return n;
}

void __fastcall HookedSinks(void* self);
cvars::SinksFn g_origSinks = nullptr;

void FindManager() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    Log("game exe timestamp 0x%08x (%s)", cvars::PeTimestamp(base),
        cvars::PeTimestamp(base) == FST_KNOWN_PE_TIMESTAMP ? "the tested build" : "NOT the tested build");
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (%d of %d matches), manager %p", singleton, votes, matches, manager);
    if (!manager || !cvars::HasModuleVtable(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, IsVariableBool at +0x%x, values %d / %g", aniso,
        screen, cvars::g_isVariableBool, a, s);
    if (!reads || a < 1 || a > 16 || !(s >= 10 && s <= 400)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_state);
        g_manager = manager;
    }
    if (!cvars::HookSinks(base, manager, &HookedSinks, &g_origSinks)) {
        g_liveError = "couldn't reach the game thread (see the log)";
        Log("game-thread hook: CallAllConsoleVariableSinks slot not found");
        g_live = Live::Failed;
        return;
    }
    g_hooked = true;
    Log("game-thread hook in place (CallAllConsoleVariableSinks was %p)", reinterpret_cast<void*>(g_origSinks));
    g_live = Live::Ready;
}

// Puts <value> in force at console priority. Game thread only.
bool SetVar(const std::wstring& key, const std::wstring& value, uint32_t priority = cvars::kSetByConsole) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str(), priority)) return false;
    std::wstring now;  // on the game thread the getters read the main value, just set
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        if (g_rejected.insert(key).second)
            Log("%ls = %ls not taken (reads %ls, priority 0x%02x)", key.c_str(), value.c_str(), now.c_str(),
                cvars::GetFlags(v.var) >> 24);
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// ---------------------------------------------------------------- HDR peak
//
// The game never sets the HDR peak. HDRSettingChangedSinkCallback (a cvar sink, on the game
// thread) builds the tone curve for UGameUserSettings::GetCurrentHDRDisplayNits(), which
// returns HDRDisplayOutputNits only while bUseHDRDisplayOutput is set, and 0 otherwise - and
// the sink then uses 1000. The game keeps its own HDR switch elsewhere and leaves that flag off,
// so the curve is always built for 1000 nits (the same as Until Dawn, docs/FINDINGS.md).
// So: the peak goes into GEngine->GameUserSettings->HDRDisplayOutputNits, the flag is set
// while HDR output is on, and r.HDR.Display.MaxLuminance (a variable the sink watches) is held
// at the same value, which makes the sink rebuild the curve. Turning the option off puts the
// game's flag and nits back. The menu's HDR sliders (mid-grey, black level, UI) still apply.
//
// The game may save the settings object to GameUserSettings.ini while the flag is set; the
// add-on writes the game's values back whenever the option is off.

constexpr uint32_t kGEngineRva = 0x8da0c30;    // ?GEngine@@3PEAVUEngine@@EA
constexpr uint32_t kGameUserSettings = 0x2b0;  // UEngine::GameUserSettings (HDRSettingChangedSinkCallback)
constexpr uint32_t kUseHdr = 0x10c;            // UGameUserSettings::bUseHDRDisplayOutput (bool)
constexpr uint32_t kHdrNits = 0x110;           // UGameUserSettings::HDRDisplayOutputNits (int)
constexpr uint32_t kSinkNitsRva = 0x8da2164;   // the sink's copy of MaxLuminance, stored after each rebuild

struct Peak {
    bool knownBuild = false;
    bool forced = false;  // our flag/nits are in the settings object
    uint8_t origFlag = 0;
    int origNits = 0;
    bool active = false;  // HDR output on and a peak chosen
} g_peak;

uint8_t* UserSettings() {
    if (!g_peak.knownBuild) return nullptr;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    uint8_t* engine = nullptr;
    uint8_t* settings = nullptr;
    if (!cvars::SafeCopy(&engine, base + kGEngineRva, 8) || !engine) return nullptr;
    if (!cvars::SafeCopy(&settings, engine + kGameUserSettings, 8) || !settings) return nullptr;
    return settings;
}

int SinkNits() {
    if (!g_peak.knownBuild) return 0;
    int n = 0;
    cvars::SafeCopy(&n, reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)) + kSinkNitsRva, 4);
    return n;
}

// Brings the settings object in line with the option; true while our peak should be in force.
bool UpdatePeak() {
    bool hdr = ReadInt(L"r.HDR.EnableHDROutput") > 0;
    bool want = g_enabled && g_hdrPeak > 0 && hdr;
    uint8_t* settings = UserSettings();
    if (!settings) return g_peak.active = false;
    uint8_t flag = 0;
    int nits = 0;
    if (!cvars::SafeCopy(&flag, settings + kUseHdr, 1) || !cvars::SafeCopy(&nits, settings + kHdrNits, 4))
        return g_peak.active = false;
    if (want) {
        if (!g_peak.forced) {
            g_peak.forced = true;
            g_peak.origFlag = flag;
            g_peak.origNits = nits;
            Log("HDR peak: game had bUseHDRDisplayOutput %u, HDRDisplayOutputNits %d", flag, nits);
        }
        if (flag != 1 || nits != g_hdrPeak) {
            uint8_t one = 1;
            cvars::SafeCopy(settings + kUseHdr, &one, 1);
            cvars::SafeCopy(settings + kHdrNits, &g_hdrPeak, 4);
            Log("HDR peak: %d nits written to the game's settings", g_hdrPeak);
        }
    } else if (g_peak.forced) {
        g_peak.forced = false;
        cvars::SafeCopy(settings + kUseHdr, &g_peak.origFlag, 1);
        cvars::SafeCopy(settings + kHdrNits, &g_peak.origNits, 4);
        Log("HDR peak: game's values back (flag %u, %d nits)", g_peak.origFlag, g_peak.origNits);
    }
    return g_peak.active = want;
}

// ---------------------------------------------------------------- apply / hold

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    if (g_peak.active) m[L"r.HDR.Display.MaxLuminance"] = std::to_wstring(g_hdrPeak);
    return m;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's value and priority.
void Apply() {
    UpdatePeak();  // settings object first: the sink reads it when MaxLuminance changes
    std::map<std::wstring, std::wstring> want = Wanted();
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) g_originals[key] = old;
            if (VarInfo v = Lookup(key); v.var) g_origPriority[key] = cvars::GetFlags(v.var) & cvars::kPriorityMask;
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        auto it = g_originals.find(key);
        if (it == g_originals.end()) continue;
        uint32_t pri = g_origPriority.count(key) ? g_origPriority[key] : cvars::kSetByCode;
        // Back at console priority (ours), then the game's priority so its menu works again.
        if (SetVar(key, it->second)) {
            if (VarInfo v = Lookup(key); v.var) cvars::RestorePriority(v.var, pri);
            ++restored;
        }
    }
    g_applied = std::move(want);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
}

// Every second: puts back any value that changed (nothing in the game should outrank console
// priority, so this is a safety net), and keeps the HDR peak in the settings object.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    bool wasActive = g_peak.active;
    if (UpdatePeak() != wasActive) {  // HDR switched on or off in the game
        g_dirty = true;
        return;
    }
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        if (SetVar(key, value)) ++g_reapplied;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (it read %ls, priority 0x%02x; %d times so far)", key.c_str(), value.c_str(),
                cur.c_str(), cvars::GetFlags(v.var) >> 24, n);
    }
}

// Present thread: starts the search for the console manager (and the hook) once.
void LiveOnPresent() {
    if (g_live.load() != Live::Idle) return;
    g_live = Live::Searching;
    std::thread(FindManager).detach();
}

// Game thread, once per frame, before the game's sinks run: all setting happens here.
void GameTick() {
    ++g_ticks;
    if (g_live.load() != Live::Ready) return;
    std::unique_lock<std::recursive_mutex> lock(g_state, std::try_to_lock);
    if (!lock.owns_lock()) return;  // the overlay is busy: next frame
    for (auto& [key, value] : g_pendingConsole) {
        bool ok = SetVar(key, value);
        Log("console: %ls = %ls %s", key.c_str(), value.c_str(), ok ? "set" : "NOT taken");
    }
    g_pendingConsole.clear();
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys = {L"r.HDR.EnableHDROutput",        L"r.HDR.Display.MaxLuminance",
                                       L"r.HDR.Display.MidLuminance",   L"r.HDR.Display.MinLuminanceLog10",
                                       L"r.HDR.UI.Luminance",           L"r.HDR.Display.OutputDevice",
                                       L"r.Lumen.HardwareRayTracing",   L"r.RayTracing",
                                       L"r.RayTracing.ForceAllRayTracingEffects", L"r.Shadow.Virtual.Enable",
                                       L"sg.GlobalIlluminationQuality", L"sg.ReflectionQuality"};
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s, priority 0x%02x, %d callbacks) = %ls", key.c_str(), cvars::TypeName(info.type),
                info.var ? cvars::GetFlags(info.var) >> 24 : 0, info.var ? Callbacks(info.var) : 0,
                Read(key, v) ? v.c_str() : L"(not found)");
        }
        uint8_t* settings = UserSettings();
        uint8_t flag = 0;
        int nits = 0;
        if (settings && cvars::SafeCopy(&flag, settings + kUseHdr, 1) && cvars::SafeCopy(&nits, settings + kHdrNits, 4))
            Log("game user settings %p: bUseHDRDisplayOutput %u, HDRDisplayOutputNits %d; HDR code last saw a peak of %d nits",
                settings, flag, nits, SinkNits());
        else
            Log("game user settings not found (%s build)", g_peak.knownBuild ? "known" : "unknown");
    }
    if (g_dirty.exchange(false)) Apply();
    Hold();
}

void __fastcall HookedSinks(void* self) {
    GameTick();
    g_origSinks(self);  // then the game's sinks, which see what GameTick set
}

// The game's value of <name> as text: the one we saved if we're overriding it, else the current.
std::string GameValue(const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = g_originals.find(name);
    if (it != g_originals.end() && g_applied.count(name)) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

// ---------------------------------------------------------------- presets
//
// A preset sets the Ray tracing and Detail options (option key -> choice id); every other option
// on those tabs goes to Game. Camera effects (taste) and HDR are left alone. The preset shown is
// derived from the options, "Custom" when none matches.
//
// Costs measured 2026-10-26 with "Measure preset costs": RTX 5080, 4K, DLSS Quality, Frame
// Generation, the menu's Ultra with Ray Tracing on, one scene. Average displayed frame rate:
// Game 144.3, Ultra+ 134.5 (+7% frame time), Cinematic 84.0 (+72%). docs/FINDINGS.md.

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker ("" = none)
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

const QualityPreset kPresets[] = {
    {"Game", "", "The game's own settings: everything on the Ray tracing and Detail tabs at Game.", {}},
    {"Ultra+", "about 7% slower",
     "The cheaper upgrades past the menu's Ultra: 6 GB texture memory, 16x filtering, 2x view\n"
     "distance, finer fog, and the game's Cinematic lamp soft shadows, hair and sky.\n"
     "Ray tracing stays as the menu sets it.",
     {{"TexturePool", 6144}, {"Anisotropy", 16}, {"ViewDistance", 2}, {"FogCells", 6}, {"SoftShadows", 1},
      {"HairDetail", 1}, {"SkyDetail", 1}}},
    {"Cinematic-lite", "",
     "Cinematic without its costly global illumination: High GI (bigger lighting cache\n"
     "budget, Cinematic fog, glass and hair lighting), the game's Cinematic reflections,\n"
     "8 GB texture memory, 16x filtering, Cinematic (10x) view distance, the finest fog, and\n"
     "the Cinematic lamp soft shadows, hair and sky. Not measured yet.",
     {{"CineGi", 2}, {"CineReflections", 1}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"ViewDistance", 10},
      {"FogCells", 4}, {"SoftShadows", 1}, {"HairDetail", 1}, {"SkyDetail", 1}}},
    {"Cinematic", "about 70% slower",
     "Everything at its highest: the game's Cinematic global illumination and reflections, 8 GB\n"
     "texture memory, 16x filtering, Cinematic (10x) view distance, the finest fog, and the\n"
     "Cinematic lamp soft shadows, hair and sky. For photo mode or a lot of GPU headroom.",
     {{"CineGi", 1}, {"CineReflections", 1}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"ViewDistance", 10},
      {"FogCells", 4}, {"SoftShadows", 1}, {"HairDetail", 1}, {"SkyDetail", 1}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

bool InPresets(const Option& o) { return !strcmp(o.tab, "Ray tracing") || !strcmp(o.tab, "Detail"); }

int PresetValue(const QualityPreset& p, const char* key) {
    for (auto& [k, id] : p.values)
        if (!strcmp(key, k)) return id;
    return 0;
}

// The preset the options match, or -1 (Custom).
int CurrentPreset() {
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const Option& o : g_options)
            if (InPresets(o) && o.value != PresetValue(kPresets[i], o.key)) match = false;
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (Option& o : g_options)
        if (InPresets(o)) o.value = PresetValue(kPresets[index], o.key);
    g_dirty = true;
}

// ---------------------------------------------------------------- frame time
//
// Present-to-present times while the game has focus. With DLSS Frame Generation on, every
// other present is a generated frame, so the rate shown is the displayed one. The average,
// not the median: with Frame Generation the intervals alternate short and long (DirectiveTweaks
// found the median picked one side).

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// Cost measurement: each preset in turn, settle then sample; then the options as they were.
// The settle time covers the game applying the change on its next frame and any stall while
// it rebuilds lighting caches or shaders.
constexpr ULONGLONG kSettleMs = 6000, kSampleMs = 8000;
struct Measure {
    bool running = false;
    int stage = 0;  // preset index being measured
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;  // option values before
    float resultMs[kPresetCount] = {};
    bool done = false;
    std::string note;
} g_measure;

void StartMeasure() {
    g_measure = {};
    g_measure.running = true;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stageStart = GetTickCount64();
    ApplyPreset(0);
    Log("measure: started");
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

// Present thread. Takes g_state (options and g_measure are shared with the overlay and the game
// thread).
void MeasureOnFrame(float ms, bool focused) {
    std::lock_guard<std::recursive_mutex> lock(g_state);
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    if (g_live.load() != Live::Ready) return FinishMeasure("stopped: the game's settings aren't reachable");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= kSettleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < kSampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", kPresets[g_measure.stage].label, avg, 1000.0f / avg,
        g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < kPresetCount) {
        ApplyPreset(g_measure.stage);
        return;
    }
    g_measure.done = true;
    char line[200];
    int n = snprintf(line, sizeof(line), "measure: result");
    for (int i = 1; i < kPresetCount; ++i)
        n += snprintf(line + n, sizeof(line) - n, ", %s %+.0f%% frame time", kPresets[i].label,
                      (g_measure.resultMs[i] / g_measure.resultMs[0] - 1) * 100);
    Log("%s", line);
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (!focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- present

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb  ? "sRGB"
                                                                               : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled.load();
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(pid == GetCurrentProcessId());
    LiveOnPresent();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// A marker before a control whose value differs from the game's.
void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

std::string ChoiceLabel(const Option& o, const Choice& c) {
    if (c.id != 0) return c.label;
    std::string v = GameValue(o.shown);
    return v.empty() ? "Game" : "Game (" + v + ")";
}

// One option's combo; true if it changed.
bool DrawOption(Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

// All options of a tab, grouped under their section headings; true if one changed.
bool DrawTab(const char* tab) {
    bool changed = false;
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        changed |= DrawOption(o);
    }
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

void DrawRayTracingStatus() {
    int rt = ReadInt(L"r.Lumen.HardwareRayTracing");
    if (rt < 0) return;
    // While we hold nothing on it, this is the menu's choice.
    if (rt)
        ImGui::TextColored(kGood, "Ray Tracing is on in the game's menu.");
    else
        ImGui::TextColored(kWarn, "Ray Tracing is off in the game's menu: reflections need it on.");
}

void DrawHdr() {
    const bool hdr = ReadInt(L"r.HDR.EnableHDROutput") > 0;
    if (!hdr) ImGui::TextColored(kWarn, "The game is in SDR. Turn on HDR in its display settings for this tab.");
    ImGui::SeparatorText("Peak brightness");
    bool changed = false;
    bool custom = g_hdrPeak != 0;
    ChangedMark(custom);
    if (ImGui::Checkbox("Set peak brightness (game: 1000 nits)", &custom)) {
        g_hdrPeak = custom ? 1000 : 0;
        changed = true;
    }
    Help("The brightest highlight the game's tone curve aims for. The game has no setting\n"
         "for it and always builds the curve for 1000 nits. Set it to your display's peak\n"
         "brightness (for example from Windows' HDR Calibration app). The menu's HDR\n"
         "sliders (brightness, black level, UI) still apply on top.");
    if (custom) {
        ImGui::TextUnformatted(" ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ControlWidth());
        // Applied when the slider is let go: each change rebuilds the game's tone curve.
        static int edit = 0;
        static bool editing = false;
        if (!editing) edit = g_hdrPeak;
        ImGui::SliderInt("Peak brightness", &edit, 400, 10000, "%d nits", ImGuiSliderFlags_AlwaysClamp);
        editing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && edit != g_hdrPeak) {
            g_hdrPeak = edit;
            changed = true;
        }
    }
    if (!g_peak.knownBuild)
        ImGui::TextColored(kWarn, "This game version isn't the one tested: the peak can't be changed.");
    else if (hdr)
        ImGui::TextDisabled("The game's HDR code last picked up a peak of %d nits.", SinkNits());

    ImGui::SeparatorText("The game's HDR menu");
    float mid = ReadFloat(L"r.HDR.Display.MidLuminance", 0);
    float minLog = ReadFloat(L"r.HDR.Display.MinLuminanceLog10", -99);
    float ui = ReadFloat(L"r.HDR.UI.Luminance", 0);
    if (mid > 0)
        ImGui::TextDisabled("Mid-grey %.1f nits (paper white about %.0f nits), black level %.4f nits, UI %.0f nits", mid,
                            mid / 0.18f, std::pow(10.0f, minLog), ui);
    ImGui::TextDisabled("These are set by the game's own HDR sliders.");
    if (changed) {
        Log("HDR peak option: %d", g_hdrPeak);
        SaveSettings();
        g_dirty = true;
    }
}

char g_cvarName[128] = "r.Lumen.ScreenProbeGather.DownsampleFactor";
char g_cvarValue[64] = "";
std::string g_cvarResult;
char g_dumpPrefix[64] = "r.Lumen.";

// Writes every console variable whose name starts with <prefix> to the log, with its value.
// The names come from the game's read-only data (UTF-16 strings), so nothing is enumerated.
void DumpPrefix(const char* prefix) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    cvars::Section rdata;
    if (!cvars::FindSection(base, ".rdata", rdata)) return;
    std::wstring p(prefix, prefix + strlen(prefix));
    std::set<std::wstring> names;
    const size_t n = p.size() * 2;
    for (size_t i = 2; i + n < rdata.size; i += 2) {
        if (rdata.begin[i - 2] || rdata.begin[i - 1] || memcmp(rdata.begin + i, p.data(), n)) continue;
        const wchar_t* s = reinterpret_cast<const wchar_t*>(rdata.begin + i);
        size_t len = 0;
        while (len < 120 && s[len] > 32 && s[len] < 127) ++len;
        if (s[len] == 0) names.insert(std::wstring(s, len));
    }
    int found = 0;
    for (const std::wstring& name : names) {
        VarInfo v = Lookup(name);
        std::wstring value;
        if (!v.var) continue;
        ++found;
        Log("  %ls (%s, priority 0x%02x) = %ls", name.c_str(), cvars::TypeName(v.type), cvars::GetFlags(v.var) >> 24,
            Read(name, value) ? value.c_str() : L"?");
    }
    Log("%d console variables starting with %s", found, prefix);
    g_cvarResult = std::to_string(found) + " settings written to FrankStoneTweaks.log";
}

void DrawAdvanced() {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Game thread: %s, %llu frames", g_hooked ? "reached" : "not reached",
                        static_cast<unsigned long long>(g_ticks.load()));
    ImGui::TextDisabled("Holding %zu settings; put back %d times after they changed", g_applied.size(), g_reapplied);
    if (!g_rejected.empty()) {
        std::string list;
        for (const std::wstring& k : g_rejected) list += (list.empty() ? "" : ", ") + Narrow(k);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
    }
    ImGui::TextDisabled("Log: FrankStoneTweaks.log next to the game's exe");
    if (live != Live::Ready) return;

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s, priority 0x%02x, %d callbacks%s", cvars::TypeName(v.type),
                            Read(name, value) ? Narrow(value).c_str() : "?", cvars::GetFlags(v.var) >> 24,
                            Callbacks(v.var), g_applied.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        g_pendingConsole.push_back({name, val});  // set by the game thread at its next frame
        g_cvarResult = "Sent to the game (the log says whether it was taken)";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value now, at console priority, like the game's console.\n"
         "It isn't saved.");
    ImGui::SetNextItemWidth(ControlWidth());
    ImGui::InputText("##prefix", g_dumpPrefix, sizeof(g_dumpPrefix));
    ImGui::SameLine();
    if (ImGui::Button("Write all to log")) DumpPrefix(g_dumpPrefix);
    Help("Writes every console variable whose name starts with this text, with its value,\n"
         "to FrankStoneTweaks.log.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard<std::recursive_mutex> lock(g_state);  // the game thread skips a frame meanwhile
    ImGui::TextDisabled(FST_PRODUCT " " FST_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    float ms = Mean(g_frameMs);
    if (ms > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);
    }
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");
    ImGui::Spacing();

    bool changed = false;
    const bool measuring = g_measure.running;

    // Quality preset and cost measurement.
    ImGui::BeginDisabled(measuring || live != Live::Ready);
    const int current = CurrentPreset();
    ChangedMark(current != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
        for (int i = 0; i < kPresetCount; ++i) {
            std::string item = kPresets[i].label;
            if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
            if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                ApplyPreset(i);
                changed = true;
                Log("preset: %s", kPresets[i].label);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s%s", kPresets[i].tip,
                                  *kPresets[i].cost ? "\n\nMeasured on an RTX 5080 at 4K (DLSS Quality, Frame Generation,\n"
                                                      "Ray Tracing on) in one scene: heavier scenes may cost more.\n"
                                                      "Measure preset costs checks them on your PC."
                                                    : "");
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Help("Sets the Ray tracing and Detail options together. Ultra+ costs about 7%, Cinematic\n"
         "about 70% (RTX 5080, 4K, DLSS Quality, Frame Generation, one scene); Cinematic-lite is\n"
         "Cinematic without its costly global illumination (not measured yet). Camera effects\n"
         "and HDR are left as they are. Changing an option afterwards shows Custom.\n"
         "Measure preset costs shows what each one costs on your PC.");

    if (measuring) {
        ImGui::TextColored(kAccent, "Measuring %s (%d of %d)... keep the camera still.", kPresets[g_measure.stage].label,
                           g_measure.stage + 1, kPresetCount);
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
    } else {
        ImGui::TextUnformatted(" ");
        ImGui::SameLine();
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure preset costs")) StartMeasure();
        ImGui::EndDisabled();
        Help("Runs every preset for 14 s each (6 s to settle, 8 s measured, about a minute) and\n"
             "shows how much longer each frame takes than with Game. Stand still in a typical\n"
             "scene and leave the mouse alone; the overlay can stay open. Your options come back\n"
             "afterwards. With Frame Generation on, the frame rate is the displayed one.");
        if (g_measure.done) {
            ImGui::SameLine();
            std::string r = "Game " + std::to_string(static_cast<int>(1000.0f / g_measure.resultMs[0] + 0.5f)) + " fps";
            for (int i = 1; i < kPresetCount; ++i) {
                char b[80];
                snprintf(b, sizeof(b), ", %s %.0f fps (%+.0f%% frame time)", kPresets[i].label,
                         1000.0f / g_measure.resultMs[i], (g_measure.resultMs[i] / g_measure.resultMs[0] - 1) * 100);
                r += b;
            }
            ImGui::TextDisabled("%s", r.c_str());
        } else if (!g_measure.note.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    ImGui::BeginDisabled(live != Live::Ready);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Ray tracing")) {
            DrawRayTracingStatus();
            changed |= DrawTab("Ray tracing");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Detail")) {
            changed |= DrawTab("Detail");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
            ImGui::TextDisabled("Motion blur and film grain are switches in the game's own menu.");
            changed |= DrawTab("Camera");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("HDR")) {
            DrawHdr();
            ImGui::EndTabItem();
        }
        ImGui::EndDisabled();
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced();
            ImGui::EndTabItem();
        }
        ImGui::BeginDisabled(live != Live::Ready);
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();  // measuring
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            g_peak.knownBuild =
                cvars::PeTimestamp(reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr))) == FST_KNOWN_PE_TIMESTAMP;
            LoadSettings();
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(FST_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            if (g_hooked) cvars::UnhookSinks(g_manager);
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
