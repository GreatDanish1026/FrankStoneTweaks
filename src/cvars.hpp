// Finds Unreal console variables in the running game and reads and sets them, for The Casting
// of Frank Stone (UE 5.1.1). The interface follows DirectiveTweaks. Layouts were read from the
// linker map the game ships (tools/mapsyms.py, docs/FINDINGS.md "Console variables").
//
// IConsoleManager::Singleton is found from the game's own code: cvar lookups load it with
// `mov rcx, [rip+Singleton]` shortly before `lea rdx, [rip+L"r.ScreenPercentage"]`. A few
// unrelated matches exist, so the address most matches agree on wins.
//
// Vtable offsets (??_7FConsoleManager@@6B@, ??_7?$FConsoleVariable@H@@6B@):
//   IConsoleManager  +0xb8  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//   IConsoleObject   +0x18  GetFlags; IsVariableBool/Int/Float/String found by reading their
//                           code (FindTypeSlots; +0x30 in this build), AsVariableBool/Int/Float
//                           follow them (TConsoleVariableData<T>*, null for Ref cvars)
//   IConsoleVariable +0x80  Set(const TCHAR* Value, EConsoleVariableFlags SetBy)
//                    +0x88  GetBool  +0x90 GetInt  +0x98 GetFloat
//
// What Set does in 5.1 (FConsoleVariable<int>::Set, exe 0x142adeb10):
//   1. CanChange: refuses a SetBy priority lower than the last one (the flags' top byte).
//   2. Stores the main value, then OnCVarChange passes it to the renderer - on the game thread
//      as a render command, so it lands between frames; off the game thread not at all.
//      It also sets FConsoleManager::bCallAllConsoleVariableSinks (+0xe0), so the sinks
//      (scalability, HDR tone curve) run at the next CallAllConsoleVariableSinks.
//   3. Records the priority and broadcasts the variable's change delegate.
// So every Set must run on the game thread: the add-on calls it from its own
// CallAllConsoleVariableSinks (HookSinks, once per frame from FEngineLoop::Tick). 0.1.0 set from
// ReShade's present thread and copied the value to the renderer itself, mid-frame; Lumen's
// resource sizes changing inside a frame is the likely cause of the GPU crash it hit.
// The add-on sets at console priority (0x0a), above the game's own code sets (0x09), so the
// menu can't undo a held value; letting go puts the game's value and priority back
// (RestorePriority).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0xb8;
constexpr uint32_t kSet = 0x80;
constexpr uint32_t kGetInt = 0x90;
constexpr uint32_t kGetFloat = 0x98;
constexpr uint32_t kFlags = 0x18;               // EConsoleVariableFlags; top byte = SetBy priority
constexpr uint32_t kSetByCode = 0x09000000;     // the game's menu code (UGameSettingsSMG037)
constexpr uint32_t kSetByConsole = 0x0a000000;  // FConsoleManager::ProcessUserConsoleInput
constexpr uint32_t kPriorityMask = 0xff000000;

// IsVariableBool's slot; Int, Float and String follow it, then AsVariableBool/Int/Float.
// Set by FindTypeSlots.
inline uint32_t g_isVariableBool = 0;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

inline uint32_t PeTimestamp(const uint8_t* base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->FileHeader.TimeDateStamp;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

// IConsoleManager::Singleton (the variable holding the pointer), or null. *votes gets how
// many of the matches agreed, *matches how many there were.
inline void** FindConsoleManagerSingleton(const uint8_t* base, int* votes = nullptr, int* matches = nullptr) {
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return nullptr;
    const uint8_t* name = FindWideString(rdata, L"r.ScreenPercentage");
    if (!name) return nullptr;
    std::map<void**, int> found;
    int total = 0;
    for (size_t i = 0x40; i + 7 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea r64, [rip+disp32] -> the name
        if ((p[0] != 0x48 && p[0] != 0x4c) || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        if (p + 7 + disp != name) continue;
        // mov rcx, [rip+disp32] within the 0x40 bytes before it
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            ++found[reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp))];
            ++total;
            break;
        }
    }
    void** best = nullptr;
    int bestVotes = 0;
    for (auto& [addr, n] : found)
        if (n > bestVotes) best = addr, bestVotes = n;
    if (votes) *votes = bestVotes;
    if (matches) *matches = total;
    return best;
}

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// memcpy that reports a fault instead of raising it.
inline bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True if the object's vtable lies in the module's read-only data (a sanity check before any
// virtual call on a pointer we found ourselves).
inline bool HasModuleVtable(const uint8_t* base, const void* object) {
    Section rdata;
    const uint8_t* vtable = nullptr;
    if (!object || !FindSection(base, ".rdata", rdata) || !SafeCopy(&vtable, object, 8)) return false;
    return vtable >= rdata.begin && vtable < rdata.begin + rdata.size;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using IntFn = int32_t(__fastcall*)(void* self);
using FloatFn = float(__fastcall*)(void* self);
using SetFn = void(__fastcall*)(void* self, const wchar_t* value, uint32_t setBy);

// The IConsoleVariable object for <name> (null if absent or a command). Guarded: a wrong
// vtable layout faults here instead of taking the game down.
inline void* FindVariable(void* manager, const wchar_t* name) {
    __try {
        return Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// What the function in <object>'s vtable at <offset> returns, read from its code: 1 for
// `return true`, 0 for `return false`, -1 for anything else. Nothing is called.
inline int ConstantReturn(const void* object, uint32_t offset) {
    __try {
        const uint8_t* fn = *reinterpret_cast<const uint8_t* const*>(*reinterpret_cast<const uint8_t* const*>(object) + offset);
        if (fn[0] == 0xe9) {  // jmp thunk
            int32_t disp;
            memcpy(&disp, fn + 1, 4);
            fn += 5 + disp;
        }
        if (fn[0] == 0xb0 && fn[2] == 0xc3) return fn[1] == 1 ? 1 : fn[1] == 0 ? 0 : -1;  // mov al, imm8; ret
        if ((fn[0] == 0x32 || fn[0] == 0x30 || fn[0] == 0x33 || fn[0] == 0x31) && fn[1] == 0xc0 && fn[2] == 0xc3)
            return 0;  // xor al/eax, al/eax; ret
        if (fn[0] == 0xb8 && fn[5] == 0xc3) {  // mov eax, imm32; ret
            uint32_t v;
            memcpy(&v, fn + 1, 4);
            return v == 1 ? 1 : v == 0 ? 0 : -1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

// Finds the four IsVariable* slots from a known int and a known float variable: the slot where
// the int's code reads false, true, false, false and the float's false, false, true, false.
inline bool FindTypeSlots(const void* intVar, const void* floatVar) {
    for (uint32_t s = 0x28; s <= 0x60; s += 8) {
        int i[4], f[4];
        for (int k = 0; k < 4; ++k) {
            i[k] = ConstantReturn(intVar, s + 8 * k);
            f[k] = ConstantReturn(floatVar, s + 8 * k);
        }
        if (i[0] == 0 && i[1] == 1 && i[2] == 0 && i[3] == 0 && f[0] == 0 && f[1] == 0 && f[2] == 1 && f[3] == 0) {
            g_isVariableBool = s;
            return true;
        }
    }
    return false;
}

enum class Type { Unknown, Bool, Int, Float, String };

inline const char* TypeName(Type t) {
    switch (t) {
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::String: return "string";
        default: return "?";
    }
}

// The value type, read from the IsVariable* slots' code (no calls).
inline Type GetType(const void* var) {
    if (!g_isVariableBool || !var) return Type::Unknown;
    if (ConstantReturn(var, g_isVariableBool + 0x10) == 1) return Type::Float;
    if (ConstantReturn(var, g_isVariableBool + 0x08) == 1) return Type::Int;
    if (ConstantReturn(var, g_isVariableBool) == 1) return Type::Bool;
    if (ConstantReturn(var, g_isVariableBool + 0x18) == 1) return Type::String;
    return Type::Unknown;
}

inline bool GetFloat(void* var, float& out) {
    __try {
        out = Virtual<FloatFn>(var, kGetFloat)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool GetInt(void* var, int32_t& out) {
    __try {
        out = Virtual<IntFn>(var, kGetInt)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The current value as text (as Set accepts it), or false. Off the game thread the getters
// return the renderer's copy.
inline bool GetText(void* var, Type t, wchar_t* out, size_t n) {
    if (t == Type::Float) {
        float v;
        if (!GetFloat(var, v)) return false;
        swprintf(out, n, L"%g", v);
        return true;
    }
    if (t == Type::Int || t == Type::Bool) {
        int32_t v;
        if (!GetInt(var, v)) return false;
        swprintf(out, n, L"%d", v);
        return true;
    }
    return false;
}

// The flags word (top byte: priority of the last accepted Set), or 0 if unreadable.
inline uint32_t GetFlags(void* var) {
    uint32_t f = 0;
    SafeCopy(&f, static_cast<uint8_t*>(var) + kFlags, 4);
    return f;
}

// Puts the priority byte back to <priority> (the game's, when we let go of a variable), so the
// game's menu can set it again. Only the top byte changes.
inline bool RestorePriority(void* var, uint32_t priority) {
    uint32_t f = GetFlags(var);
    f = (f & ~kPriorityMask) | (priority & kPriorityMask);
    return SafeCopy(static_cast<uint8_t*>(var) + kFlags, &f, 4);
}

// Sets at <setBy> priority. Call it on the game thread only (see the header). False if it
// faulted.
inline bool Set(void* var, const wchar_t* value, uint32_t setBy = kSetByConsole) {
    __try {
        Virtual<SetFn>(var, kSet)(var, value, setBy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------ running on the game thread
//
// FEngineLoop::Tick calls IConsoleManager::CallAllConsoleVariableSinks (vtable +0x50) through the
// manager's vtable once per frame, on the game thread. HookSinks gives the manager a copy of its
// vtable with that slot pointing at <hook>; the hook does its work and then calls the original
// (returned in *original). The copy keeps the RTTI pointer in front of it. Nothing in the
// game's code is changed, and UnhookSinks puts the original vtable back.
constexpr uint32_t kCallAllConsoleVariableSinks = 0x50;
using SinksFn = void(__fastcall*)(void* self);

inline void* g_vtableCopy[1 + 256];
inline void** g_vtableOriginal = nullptr;

inline bool HookSinks(const uint8_t* base, void* manager, SinksFn hook, SinksFn* original) {
    Section text;
    void** vt = nullptr;
    if (!FindSection(base, ".text", text) || !SafeCopy(&vt, manager, 8) || !vt) return false;
    size_t n = 0;
    while (n < 256) {  // slots up to the first one that isn't code
        const uint8_t* fn = nullptr;
        if (!SafeCopy(&fn, vt + n, 8) || fn < text.begin || fn >= text.begin + text.size) break;
        ++n;
    }
    const size_t slot = kCallAllConsoleVariableSinks / 8;
    if (n <= slot || !SafeCopy(&g_vtableCopy[0], vt - 1, 8)) return false;
    memcpy(&g_vtableCopy[1], vt, n * 8);
    *original = reinterpret_cast<SinksFn>(vt[slot]);
    g_vtableCopy[1 + slot] = reinterpret_cast<void*>(hook);
    g_vtableOriginal = vt;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(manager), &g_vtableCopy[1]);
    return true;
}

inline void UnhookSinks(void* manager) {
    if (manager && g_vtableOriginal)
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(manager), g_vtableOriginal);
    g_vtableOriginal = nullptr;
}

}  // namespace cvars
