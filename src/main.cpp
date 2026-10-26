// FrankStoneTweaks: a ReShade add-on for The Casting of Frank Stone (UE 5.1.1, DirectX 12) that
// raises the game's ray tracing past its menu and sets the HDR peak brightness, live, from an
// in-game overlay.
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, UE 5.1 layout read from the linker map the game ships). Values are set at console
// priority, above the game's own menu code, so they stay until the option goes back to Game;
// then the game's value and priority are put back. A check every second puts back anything
// that changed anyway. F8 switches all of it off and on for comparison.
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
    "Cinematic ray tracing, ray traced shadows and HDR peak brightness for " FST_GAME ", live from the overlay.";

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
     "Lumen's bounce lighting. Cinematic is the game's own unused tier above Ultra:\n"
     "lighting probes every 8 pixels instead of 16 (4x as many), twice the directions\n"
     "traced per probe, over 3x the probe budget, finer lighting in fog and on hair and\n"
     "glass. Softer, more stable indirect light with less blotching and leaking.\n"
     "Applies with or without the menu's Ray Tracing.",
     L"r.Lumen.ScreenProbeGather.DownsampleFactor",
     {{"Game", 0, {}},
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
    {"Ray tracing", "Shadows", "RtShadows", "Ray traced shadows",
     "Shadows from ray tracing instead of shadow maps: contact-hardening (sharp near the\n"
     "object, softer further away) and no shadow-map blockiness or light leaking. The\n"
     "engine supports them but the game ships with them off.",
     L"r.RayTracing.Shadows",
     {{"Game", 0, {}}, {"On", 1, {{L"r.RayTracing.Shadows", L"1"}}}}},
    {"Ray tracing", "Shadows", "RtRange", "Ray tracing range",
     "The game leaves small objects out of the ray tracing scene beyond 50 m and behind\n"
     "the camera, so their ray traced shadows and reflections can vanish or pop.\n"
     "Everything keeps all objects in (more ray tracing work in large scenes).",
     L"r.RayTracing.Culling",
     {{"Game", 0, {}}, {"Everything", 1, {{L"r.RayTracing.Culling", L"0"}}}}},
};

// HDR peak brightness in nits (0 = the game's).
int g_hdrPeak = 0;
bool g_enabled = true;  // F8

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
bool g_dirty = true;                               // options changed: apply at the next present
int g_reapplied = 0;                               // values put back after they changed

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
    cvars::Storage st;
    bool storage = reads && cvars::GetStorage(aniso, st);
    if (!reads || !storage || a < 1 || a > 16 || !(s >= 10 && s <= 400)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    Log("r.MaxAnisotropy storage: %s, main %p, renderer %p", st.ref ? "ref" : "data", st.main, st.render);
    g_manager = manager;
    g_live = Live::Ready;
}

// Puts <value> in force at console priority. Variables with change callbacks get the value
// written straight into their storage instead: Set would run those callbacks on this (not the
// game's) thread. The sink flag is raised either way so the game thread's sinks see the change.
bool SetVar(const std::wstring& key, const std::wstring& value, uint32_t priority = cvars::kSetByConsole) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    int callbacks = Callbacks(v.var);
    bool ok;
    if (callbacks != 0) {
        cvars::Storage s;
        ok = cvars::GetStorage(v.var, s);
        if (ok) {
            uint8_t raw[4] = {};
            if (s.type == cvars::Type::Float) {
                float f = static_cast<float>(_wtof(value.c_str()));
                memcpy(raw, &f, 4);
            } else {
                int32_t i = _wtoi(value.c_str());
                memcpy(raw, &i, 4);
            }
            ok = cvars::SafeCopy(const_cast<uint8_t*>(s.main), raw, s.size) && cvars::SafeCopy(s.render, raw, s.size) &&
                 cvars::RestorePriority(v.var, priority);
            if (g_manager) {
                uint8_t one = 1;
                cvars::SafeCopy(static_cast<uint8_t*>(g_manager) + 0xe0, &one, 1);  // bCallAllConsoleVariableSinks
            }
        }
        static std::set<std::wstring> logged;
        if (logged.insert(key).second)
            Log("%ls has %d change callback(s): written directly (callbacks not run)", key.c_str(), callbacks);
    } else {
        ok = cvars::Set(v.var, value.c_str(), priority);
    }
    if (!ok) return false;
    std::wstring now;
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
constexpr uint32_t kSinkNitsRva = 0x8da2164;   // the nits the sink last built the curve for

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

void LiveOnPresent() {
    Live state = g_live.load();
    if (state == Live::Idle) {
        g_live = Live::Searching;
        std::thread(FindManager).detach();
        return;
    }
    if (state != Live::Ready) return;
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
            Log("game user settings %p: bUseHDRDisplayOutput %u, HDRDisplayOutputNits %d; tone curve built for %d nits",
                settings, flag, nits, SinkNits());
        else
            Log("game user settings not found (%s build)", g_peak.knownBuild ? "known" : "unknown");
    }
    if (g_dirty) {
        g_dirty = false;
        Apply();
    }
    Hold();
}

// The game's value of <name> as text: the one we saved if we're overriding it, else the current.
std::string GameValue(const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = g_originals.find(name);
    if (it != g_originals.end() && g_applied.count(name)) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

// ---------------------------------------------------------------- frame time
//
// Present-to-present times while the game has focus. With DLSS Frame Generation on, every
// other present is a generated frame, so the rate shown is the displayed one. The average,
// not the median: with Frame Generation the intervals alternate short and long.

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

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) return;
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
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
        g_enabled = !g_enabled;
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
        ImGui::TextDisabled("The game's tone curve is built for %d nits.", SinkNits());

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

char g_cvarName[128] = "r.RayTracing.Shadows";
char g_cvarValue[64] = "";
std::string g_cvarResult;
char g_dumpPrefix[64] = "r.RayTracing.Shadows";

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
        bool ok = SetVar(name, val);
        Log("console: %ls = %ls %s", name.c_str(), val.c_str(), ok ? "set" : "NOT taken");
        g_cvarResult = ok ? "Set at console priority (the game's menu can't change it until restart)" : "Not taken";
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
    ImGui::BeginDisabled(live != Live::Ready);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Ray tracing")) {
            DrawRayTracingStatus();
            changed |= DrawTab("Ray tracing");
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
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
