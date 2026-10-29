// Reverse.dll - backing off like the reverse order of Company of Heroes, for Dawn of War II: Retribution 3.19 (Elite).
// Hold the key (Alt by default) and right-click the ground: the selected squads move straight back toward it, at most
// StepMeters (25) per click - vehicles drive backwards instead of turning around; click again to keep backing off.
//
// Why that reverses vehicles, with no change to the game: their turn plans (attrib turn_plan/tank, vehicles,
// agile_vehicle) have a "reverse" step for paths up to 30 m, so a short move back is driven backwards by the game
// itself (tested on the stand: Predator, 13 and 20 m - the hull keeps facing away from the path). A long move, or a
// queue of short ones, is planned as a turn and drive forward (tested too) - hence one step per click. The order is the
// game's own right-click (CommandProxy::DefaultAction) with the point moved closer: sent the normal way, simulated the
// same on every machine of the match - no desync, the other players don't need the DLL.
//
// Hook: CommandProxy::DefaultAction (vtable 0x110E514 #29, 0x00AF62E0, __thiscall: group, Target*, ?, Player*, queued).
// Not a ground position, or the key not held: the game's own default action, untouched.
// Reverse.ini [Reverse]: Key=Alt|Ctrl|<virtual key code>, StepMeters=25 (5..30), re-read on every order.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define DEFACT_ADDR   0x00AF62E0                     // CommandProxy::DefaultAction
static const unsigned char PROLOGUE[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};   // push ebp; mov ebp,esp; and esp,-8

typedef struct { float x, y, z; } Vec3;
typedef void (__fastcall *DefAct_t)(void *self, void *edx, void *group, void *target, int p4, void *player, int queued);
typedef int  (__fastcall *GetType_t)(void *target, void *edx);
typedef Vec3 *(__fastcall *TargetPos_t)(void *target, void *edx, Vec3 *out);
typedef void **(__fastcall *Begin_t)(void *cont, void *edx);
typedef const Vec3 *(__fastcall *EntPos_t)(void *entity, void *edx);
typedef void (__fastcall *Reset_t)(void *target, void *edx, const Vec3 *pos);

static DefAct_t o_defact;
static GetType_t p_type; static TargetPos_t p_tpos; static Begin_t p_begin, p_end; static EntPos_t p_epos; static Reset_t p_reset;
static int g_key = VK_MENU; static float g_step = 25.0f;
static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH];
static void load_ini(void);

static void logmsg(const char *fmt, ...) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsReverse.log", g_dir);
    FILE *f = _wfopen(p, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

// the entities of an EntityGroupTemp: its ObservableContainer<Entity const*> is the virtual base (offset in the vbtable)
static int group_center(void *group, Vec3 *c) {
    int *vbt = *(int **)group; if (IsBadReadPtr(vbt, 8)) return 0;
    void *cont = (char *)group + vbt[1];
    void **b = p_begin(cont, NULL), **e = p_end(cont, NULL); int n = 0;
    c->x = c->y = c->z = 0;
    for (void **it = b; it && it < e && n < 64; it++) {
        if (!*it) continue;
        const Vec3 *p = p_epos(*it, NULL); if (!p) continue;
        c->x += p->x; c->y += p->y; c->z += p->z; n++;
    }
    if (n) { c->x /= n; c->y /= n; c->z /= n; }
    return n;
}

static void __fastcall hk_defact(void *self, void *edx, void *group, void *target, int p4, void *player, int queued) {
    if (!(GetAsyncKeyState(g_key) & 0x8000) || !target || !group || p_type(target, NULL) != 2) {   // 2 = a ground position
        o_defact(self, edx, group, target, p4, player, queued); return;
    }
    load_ini();                                                      // re-read: tuning while the game runs
    Vec3 to, from = {0, 0, 0}; p_tpos(target, NULL, &to);
    int n = group_center(group, &from);
    float dx = to.x - from.x, dz = to.z - from.z, dist = sqrtf(dx * dx + dz * dz);
    if (!n || dist < 1.0f) { o_defact(self, edx, group, target, p4, player, queued); return; }
    // one plain move: the game plans up to ~30 m of driving backwards itself (turn plan "reverse"), but not for a queue
    // of moves (tested: the first leg then turns around too) - so a far click backs off StepMeters toward it, click again
    float k = dist > g_step ? g_step / dist : 1.0f;
    Vec3 p = { from.x + dx * k, to.y, from.z + dz * k };
    p_reset(target, NULL, &p);
    o_defact(self, edx, group, target, p4, player, queued);
    logmsg("reverse: %d squad(s), %.0f m back%s%s", n, dist * k, k < 1 ? " (clicked farther: one step)" : "", queued ? " (queued)" : "");
}

static void *sim(const char *name) { HMODULE m = GetModuleHandleA("SimEngine.dll"); return m ? (void *)GetProcAddress(m, name) : NULL; }
static int install(void) {
    p_type  = (GetType_t)  sim("?GetType@ConstTarget@@QBE?AW4TargetType@1@XZ");
    p_tpos  = (TargetPos_t)sim("?GetPosition@ConstTarget@@QBE?BVVector3f@Math@@XZ");
    p_begin = (Begin_t)    sim("?begin@?$ObservableContainer@PBVEntity@@$00@ast@@QAEPAPBVEntity@@XZ");
    p_end   = (Begin_t)    sim("?end@?$ObservableContainer@PBVEntity@@$00@ast@@QAEPAPBVEntity@@XZ");
    p_epos  = (EntPos_t)   sim("?GetPosition@Entity@@QBEABVVector3f@Math@@XZ");
    p_reset = (Reset_t)    sim("?Reset@ConstTarget@@QAEXABVVector3f@Math@@@Z");
    if (!p_type || !p_tpos || !p_begin || !p_end || !p_epos || !p_reset) { logmsg("SimEngine.dll functions not found - not hooking"); return 0; }
    unsigned char *t = (unsigned char *)DEFACT_ADDR;
    if (IsBadReadPtr(t, 6) || memcmp(t, PROLOGUE, 6)) {
        logmsg("DOW2.exe is not 3.19 (prologue differs) - not hooking"); return 0;
    }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, t, 6); tr[6] = 0xE9; *(DWORD *)(tr + 7) = (DWORD)(t + 6) - (DWORD)(tr + 11);
    o_defact = (DefAct_t)tr;
    DWORD o; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &o);
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)hk_defact - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 6);
    return 1;
}

static DWORD WINAPI worker(LPVOID _) {
    for (int i = 0; i < 60; i++) { if (GetModuleHandleA("SimEngine.dll")) break; Sleep(500); }
    logmsg("Reverse 1 start: hold %s and right-click the ground to back off up to %.0f m - hook %s",
           g_key == VK_MENU ? "Alt" : g_key == VK_CONTROL ? "Ctrl" : "the key", g_step, install() ? "installed" : "NOT installed");
    return 0;
}

static void load_ini(void) {
    wchar_t k[16]; GetPrivateProfileStringW(L"Reverse", L"Key", L"Alt", k, 16, g_ini);
    g_key = !lstrcmpiW(k, L"Alt") ? VK_MENU : !lstrcmpiW(k, L"Ctrl") ? VK_CONTROL : (int)wcstol(k, NULL, 0);
    if (g_key <= 0 || g_key > 254) g_key = VK_MENU;
    int st = GetPrivateProfileIntW(L"Reverse", L"StepMeters", 25, g_ini); if (st < 5) st = 5; if (st > 30) st = 30;
    g_step = (float)st;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    char mx[64]; wsprintfA(mx, "Local\\dow2patch_Reverse_%lu", GetCurrentProcessId());
    CreateMutexA(NULL, FALSE, mx); if (GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
    GetModuleFileNameW(h, g_dir, MAX_PATH); wchar_t *s = wcsrchr(g_dir, L'\\'); if (s) s[1] = 0;
    _snwprintf(g_ini, MAX_PATH, L"%lsReverse.ini", g_dir); load_ini();
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}
