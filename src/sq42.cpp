#include "sq42.h"
#include "menu.h"

static const struct { const char* label; const char* cvar; const char* tip; } kSettings[] = {
    { "SQ42 auto targeting", "i_target_selector.targeting2_enabled",
      "\"Enables the auto targeting feature for SQ42\" (the game mode can override it)." },
    { "Visor mini-map", "pl_lensdisplay.minimap_enabled", "The mini-map on your visor HUD." },
    { "Visor greebles", "pl_lensdisplay.greebles_enabled", "The decorative frame pieces on your visor HUD." },
    { "SQ42 menus (experimental)", "g_squadron_frontend",
      "\"Enable Squadron 42 Frontend\". Switches the pause menu and loading screens to SQ42's, whose data is "
      "missing; turn it off before traveling or quitting if anything breaks." },
};
constexpr int kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);

static int32_t*   g_storage[kSettingCount];
static uintptr_t* g_gameCVars = nullptr;
static int32_t    g_gameCVarOffset[kSettingCount];

static bool FindIntCVar(const Section& text, const Section& rdata, const char* cvar, int32_t*& storage, int32_t& gameOffset,
                        uint8_t registerSlot = 0x40) {
    const uint8_t* name = FindCString(rdata, cvar);
    uint8_t* const end = text.base + text.size - 0x30;
    for (uint8_t* p = text.base + 0x20; name && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != name) continue;
        bool registers = false;
        for (int f = 7; f <= 0x20 && !registers; ++f)
            registers = (BytesMatch(p + f, "FF 50") && p[f + 2] == registerSlot) || (BytesMatch(p + f, "4C 8B 50") && p[f + 3] == registerSlot);
        if (!registers) continue;
        for (int b = 7; b <= 0x20; ++b) {
            const uint8_t* q = p - b;
            if (BytesMatch(q, "4C 8D 05")) { storage = reinterpret_cast<int32_t*>(const_cast<uint8_t*>(q + 7 + Rel32(q + 3))); return true; }
            if (BytesMatch(q, "4C 8D 87") && g_gameCVars) { gameOffset = Rel32(q + 3); return true; }
        }
    }
    return false;
}

static void FindSetting(const Section& text, const Section& rdata, int i) {
    if (!FindIntCVar(text, rdata, kSettings[i].cvar, g_storage[i], g_gameCVarOffset[i]))
        Log("[sq42] setting %s not found", kSettings[i].cvar);
}

static const struct { const char* cvar; int32_t value; const char* also; } kKeptOn[] = {
    { "v_qdrive2.quantumTravelAllowed", 1, "p_enable_physical_quantum_travel 1" },
    { "v_qdrive2.quantumBoostAllowed", 1, nullptr },
    { "v_qdrive2.setting_ignoreBlockedBoost", 1, nullptr },
    { "v_qdrive2.setting_ignoreBlockedTravel", 1, nullptr },
    { "v_qdrive.logging", 1, nullptr },
};
constexpr int kKeptOnCount = sizeof(kKeptOn) / sizeof(kKeptOn[0]);
static int32_t* g_keptOn[kKeptOnCount];

static const struct { const char* cvar; float value; } kKeptOnFloat[] = {
    { "v_qdrive2.setting_targetLockAngularSpeedThresholdPlayer", 45.0f },
    { "v_qdrive2.setting_targetLockLinearSpeedThresholdPlayer", 1000.0f },
};
constexpr int kKeptOnFloatCount = sizeof(kKeptOnFloat) / sizeof(kKeptOnFloat[0]);
static float* g_keptOnFloat[kKeptOnFloatCount];

static int32_t* Storage(int i) {
    if (g_storage[i]) return g_storage[i];
    if (g_gameCVarOffset[i] > 0 && g_gameCVarOffset[i] < 0x4000 && g_gameCVars && *g_gameCVars)
        return reinterpret_cast<int32_t*>(*g_gameCVars + g_gameCVarOffset[i]);
    return nullptr;
}

int Menu_Sq42SettingCount() { return kSettingCount; }
const char* Menu_Sq42SettingName(int i) { return i >= 0 && i < kSettingCount ? kSettings[i].label : ""; }
const char* Menu_Sq42SettingTip(int i) { return i >= 0 && i < kSettingCount ? kSettings[i].tip : ""; }

int Menu_Sq42Setting(int i) {
    if (i < 0 || i >= kSettingCount) return -1;
    __try {
        const int32_t* s = Storage(i);
        return s ? *s : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

using ExecuteFn = void(__fastcall*)(uintptr_t console, const char* cmd, bool silent, bool deferred, void* unused);
static uintptr_t* g_console = nullptr;

static void FindConsole(const Section& text, const Section& rdata) {
    const uint8_t* str = FindCString(rdata, "debugGUI_enable 1");
    uint8_t* const end = text.base + text.size - 0x20;
    for (uint8_t* p = text.base + 7; str && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != str) continue;
        if (BytesMatch(p - 7, "48 8B 0D") && BytesMatch(p + 7, "45 33 C9") && BytesMatch(p + 0x19, "FF 90 30 01 00 00")) {
            g_console = reinterpret_cast<uintptr_t*>(p + Rel32(p - 4));
            return;
        }
    }
    Log("[sq42] console not found; the console line is disabled");
}

static SRWLOCK       g_lock = SRWLOCK_INIT;
static char          g_command[256];
static volatile LONG g_commandPending = 0;
static struct { volatile LONG pending; int index; int value; } g_set[kSettingCount];

bool Menu_ConsoleReady() { return g_console != nullptr; }

void Menu_RunConsole(const char* cmd) {
    AcquireSRWLockExclusive(&g_lock);
    strncpy_s(g_command, cmd, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_commandPending, 1);
}

void Menu_SetSq42Setting(int i, bool on) {
    if (i < 0 || i >= kSettingCount) return;
    g_set[i].value = on ? 1 : 0;
    InterlockedExchange(&g_set[i].pending, 1);
}

static bool Execute(const char* cmd) {
    if (!g_console || !*g_console) return false;
    __try {
        VCall<void>(*g_console, 0x130, cmd, false, false, static_cast<void*>(nullptr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[sq42] fault while running '%s'", cmd);
        return false;
    }
}

bool RunConsoleNow(const char* cmd) { return Execute(cmd); }

bool SetCVarNow(const char* name, float value) {
    if (!g_console || !*g_console) return false;
    __try {
        const uintptr_t cvar = VCall<uintptr_t>(*g_console, 0xC0, name);
        if (!cvar) return false;
        VCall<void>(cvar, 0x38, value);
        return VCall<float>(cvar, 0x20) == value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ResolveSq42Api(const Section& text, const Section& rdata) {
    int n = 0;
    if (uint8_t* p = FindUniquePattern(text, "48 89 83 38 01 00 00 B9 98 00 00 00 48 89 05", n))
        g_gameCVars = reinterpret_cast<uintptr_t*>(p + 0x13 + Rel32(p + 0xF));
    for (int i = 0; i < kSettingCount; ++i) FindSetting(text, rdata, i);
    for (int i = 0; i < kKeptOnCount; ++i) {
        int32_t offset = 0;
        if (!FindIntCVar(text, rdata, kKeptOn[i].cvar, g_keptOn[i], offset) || !g_keptOn[i])
            Log("[+] %s not found; it stays as the game mode sets it", kKeptOn[i].cvar);
    }
    for (int i = 0; i < kKeptOnFloatCount; ++i) {
        int32_t offset = 0;
        int32_t* storage = nullptr;
        if (FindIntCVar(text, rdata, kKeptOnFloat[i].cvar, storage, offset, 0x48) && storage)
            g_keptOnFloat[i] = reinterpret_cast<float*>(storage);
        else
            Log("[+] %s not found; it keeps its default", kKeptOnFloat[i].cvar);
    }
    FindConsole(text, rdata);
}

static void KeepSettingsOn() {
    for (int i = 0; i < kKeptOnCount; ++i) {
        int32_t* const s = g_keptOn[i];
        if (!s) continue;
        __try {
            if (*s == kKeptOn[i].value) continue;
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "%s %d", kKeptOn[i].cvar, kKeptOn[i].value);
            Execute(cmd);
            if (*s != kKeptOn[i].value) *s = kKeptOn[i].value;
            if (kKeptOn[i].also) Execute(kKeptOn[i].also);
            Log("[+] %s = %d%s%s", kKeptOn[i].cvar, *s, kKeptOn[i].also ? ", " : "", kKeptOn[i].also ? kKeptOn[i].also : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    for (int i = 0; i < kKeptOnFloatCount; ++i) {
        float* const s = g_keptOnFloat[i];
        if (!s) continue;
        __try {
            if (*s == kKeptOnFloat[i].value) continue;
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "%s %g", kKeptOnFloat[i].cvar, kKeptOnFloat[i].value);
            Execute(cmd);
            if (*s != kKeptOnFloat[i].value) *s = kKeptOnFloat[i].value;
            Log("[+] %s = %g", kKeptOnFloat[i].cvar, *s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void ProcessSq42() {
    KeepSettingsOn();
    if (InterlockedExchange(&g_commandPending, 0)) {
        char cmd[256];
        AcquireSRWLockExclusive(&g_lock);
        strcpy_s(cmd, g_command);
        ReleaseSRWLockExclusive(&g_lock);
        if (Execute(cmd)) Log("[sq42] console: %s", cmd);
        else              Log("[sq42] console unavailable: %s", cmd);
    }
    for (int i = 0; i < kSettingCount; ++i) {
        if (!InterlockedExchange(&g_set[i].pending, 0)) continue;
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "%s %d", kSettings[i].cvar, g_set[i].value);
        Execute(cmd);
        __try {
            if (int32_t* s = Storage(i); s && *s != g_set[i].value) *s = g_set[i].value;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[sq42] %s = %d (now %d)", kSettings[i].cvar, g_set[i].value, Menu_Sq42Setting(i));
    }
}
