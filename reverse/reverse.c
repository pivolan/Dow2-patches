// Reverse.dll - backing off like the reverse order of Company of Heroes, for Dawn of War II: Retribution 3.19 (Elite).
// Hold the key (Ctrl by default) and right-click the ground (or the minimap): the selected squads back off to the point,
// vehicles driving backwards with the front armour to the enemy instead of turning around.
//
// Why that reverses vehicles, with no change to the game: their turn plans (attrib turn_plan/tank, vehicles,
// agile_vehicle) have a "reverse" step for paths up to 30 m, so a short move back is driven backwards by the game itself
// (tested on the stand: Predator, 13-25 m, the hull keeps facing away from the path). A long move, or a queue of short
// ones, is planned as a turn and drive forward (tested too). So the way is driven leg by leg: StepMeters (25) at a time,
// the next leg LeadMeters (8) before the end of the current one, while still moving - no stop between the legs (a WM_TIMER
// on the game window = the game's main thread). Every leg is the game's own right-click order (CommandProxy::DefaultAction) to a nearer point - sent the
// normal way, simulated the same on every machine of the match: no desync, the other players don't need the DLL.
// The legs go straight toward the click: a wall in between stops the back-off there (as a Stop order does).
//
// Hook: CommandProxy::DefaultAction (vtable 0x110E514 #29, 0x00AF62E0, __thiscall: group, Target*, ?, Player*, queued).
// Not a ground position, or the key not held: the game's own default action, untouched - and it cancels a back-off in
// progress. So does losing the squads, or a leg that ends short (blocked, or the squads got another order).
// Reverse.ini [Reverse]: Key=Ctrl|Alt|<virtual key code>, StepMeters=25 (5..30), LeadMeters=8 (0 = wait for the stop, as in
// Reverse 3), re-read on every order.
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
typedef void (__fastcall *GrpCtor_t)(void *grp, void *edx, int mostDerived);
typedef void (__fastcall *Thiscall0_t)(void *obj, void *edx);
typedef void (__fastcall *PushBack_t)(void *cont, void *edx, void *const *ent);
typedef DWORD (__fastcall *GetID_t)(void *ent, void *edx);
typedef void **(__fastcall *GetEnts_t)(void *em, void *edx);          // STLport vector<Entity*>&: begin, end, ...
typedef void (__fastcall *TgtCtor_t)(void *tgt, void *edx, const Vec3 *pos);

static DefAct_t o_defact;
static GetType_t p_type; static TargetPos_t p_tpos; static Begin_t p_begin, p_end; static EntPos_t p_epos; static Reset_t p_reset;
static GrpCtor_t p_gctor; static Thiscall0_t p_gdtor, p_tdtor; static PushBack_t p_push; static GetID_t p_id; static GetEnts_t p_ents; static TgtCtor_t p_tctor;
#define G_WORLD 0x013356A4                          // the world; +4 = its EntityManager (as in DefaultAction)

// the rest of a long back-off: one leg at a time (a queue of legs is planned as turning around), the next one when the
// squads have stopped at the end of the current leg. Main thread only: the click hook and a WM_TIMER of the same thread.
static struct { int active, n, p4; void *self, *player, *ent[32]; DWORD id[32]; Vec3 to, legEnd, last; DWORD legAt, movedAt; } g_plan;
static int g_key = VK_CONTROL; static float g_step = 25.0f, g_lead = 8.0f;
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

static int alive_center(Vec3 *c) {                  // the plan's entities that still exist, and their center
    void **v = p_ents((char *)*(void **)G_WORLD + 4, NULL); void **b = (void **)v[0], **e = (void **)v[1];
    int k = 0; c->x = c->y = c->z = 0;
    for (int i = 0; i < g_plan.n; i++) {
        int ok = 0;
        for (void **it = b; it < e; it++) if (*it == g_plan.ent[i]) { ok = 1; break; }
        if (!ok || p_id(g_plan.ent[i], NULL) != g_plan.id[i]) continue;
        const Vec3 *p = p_epos(g_plan.ent[i], NULL);
        c->x += p->x; c->y += p->y; c->z += p->z; g_plan.ent[k] = g_plan.ent[i]; g_plan.id[k] = g_plan.id[i]; k++;
    }
    g_plan.n = k; if (k) { c->x /= k; c->y /= k; c->z /= k; }
    return k;
}
static void issue_leg(const Vec3 *from, float step) {  // a plain right-click order toward the end, at most one step
    float dx = g_plan.to.x - from->x, dz = g_plan.to.z - from->z, d = sqrtf(dx * dx + dz * dz);
    if (d < 2.0f) { g_plan.active = 0; logmsg("reverse: arrived"); return; }
    float k = d > step ? step / d : 1.0f;
    Vec3 p = { from->x + dx * k, g_plan.to.y, from->z + dz * k };
    static unsigned char grp[0x200], tgt[0x80];
    memset(grp, 0, sizeof grp); memset(tgt, 0, sizeof tgt);
    p_gctor(grp, NULL, 1);
    void *cont = grp + (*(int **)grp)[1];
    for (int i = 0; i < g_plan.n; i++) p_push(cont, NULL, &g_plan.ent[i]);
    p_tctor(tgt, NULL, &p);
    o_defact(g_plan.self, NULL, grp, tgt, g_plan.p4, g_plan.player, 0);
    p_tdtor(tgt, NULL); p_gdtor(grp, NULL);
    g_plan.legEnd = p; g_plan.legAt = g_plan.movedAt = GetTickCount(); g_plan.last = *from;
    if (k >= 1.0f) g_plan.active = 0;                // the last leg
    logmsg("reverse: next leg %.0f m, %.0f m left", d * k, d - d * k);
}
static VOID CALLBACK tick(HWND h, UINT m, UINT_PTR id, DWORD now) {
    if (!g_plan.active) return;
    Vec3 c; if (!alive_center(&c)) { g_plan.active = 0; logmsg("reverse: the squads are gone"); return; }
    float mx = c.x - g_plan.last.x, mz = c.z - g_plan.last.z;
    float ex = c.x - g_plan.legEnd.x, ez = c.z - g_plan.legEnd.z, left2 = ex * ex + ez * ez;
    if (mx * mx + mz * mz > 0.25f) {                                // still moving
        g_plan.last = c; g_plan.movedAt = now;
        // rolling legs: the next one while still backing in, LeadMeters before the end - no stop between the legs
        // (the new point is still behind the hull and under 30 m away: the game keeps driving it backwards)
        if (left2 < g_lead * g_lead) issue_leg(&c, g_step);
        return;
    }
    if (now - g_plan.movedAt < 1000) return;                       // turning the rear toward the path takes a moment
    // stopped short: the straight line runs into an obstacle, or the player stopped them (S, an ability): done. Going on
    // around the obstacle by itself would also override the player's own Stop.
    if (left2 > 8.0f * 8.0f) { g_plan.active = 0; logmsg("reverse: stopped short of the leg (blocked, or another order) - done"); return; }
    issue_leg(&c, g_step);
}

static void __fastcall hk_defact(void *self, void *edx, void *group, void *target, int p4, void *player, int queued) {
    static UINT_PTR timer;                           // this is the game's main thread: a timer on its window (its message
    if (!timer) {                                    // loop dispatches window messages; a thread timer is never picked up)
        HWND w = GetActiveWindow(); if (!w) w = GetForegroundWindow();
        timer = SetTimer(w, 0x5245, 250, tick);
        logmsg("timer %s (window %p)", timer ? "on" : "FAILED", (void *)w);
    }
    if (!(GetAsyncKeyState(g_key) & 0x8000) || !target || !group || p_type(target, NULL) != 2) {   // 2 = a ground position
        g_plan.active = 0;                           // any other order cancels a back-off in progress
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
    // the rest, leg by leg (queued clicks keep the game's own queue: no continuation then)
    memset(&g_plan, 0, sizeof g_plan);
    if (k < 1.0f && !queued) {
        int *vbt = *(int **)group; void *cont = (char *)group + vbt[1];
        void **b = p_begin(cont, NULL), **e = p_end(cont, NULL);
        for (void **it = b; it && it < e && g_plan.n < 32; it++) if (*it) { g_plan.ent[g_plan.n] = *it; g_plan.id[g_plan.n] = p_id(*it, NULL); g_plan.n++; }
        g_plan.self = self; g_plan.player = player; g_plan.p4 = p4; g_plan.to = to; g_plan.legEnd = p;
        g_plan.last = from; g_plan.legAt = g_plan.movedAt = GetTickCount(); g_plan.active = g_plan.n > 0;
    }
    logmsg("reverse: %d squad(s), %.0f m back, first leg %.0f m%s", n, dist, dist * k, queued ? " (queued: no continuation)" : "");
}

static void *sim(const char *name) { HMODULE m = GetModuleHandleA("SimEngine.dll"); return m ? (void *)GetProcAddress(m, name) : NULL; }
static int install(void) {
    p_type  = (GetType_t)  sim("?GetType@ConstTarget@@QBE?AW4TargetType@1@XZ");
    p_tpos  = (TargetPos_t)sim("?GetPosition@ConstTarget@@QBE?BVVector3f@Math@@XZ");
    p_begin = (Begin_t)    sim("?begin@?$ObservableContainer@PBVEntity@@$00@ast@@QAEPAPBVEntity@@XZ");
    p_end   = (Begin_t)    sim("?end@?$ObservableContainer@PBVEntity@@$00@ast@@QAEPAPBVEntity@@XZ");
    p_epos  = (EntPos_t)   sim("?GetPosition@Entity@@QBEABVVector3f@Math@@XZ");
    p_reset = (Reset_t)    sim("?Reset@ConstTarget@@QAEXABVVector3f@Math@@@Z");
    p_gctor = (GrpCtor_t)  sim("??0EntityGroupTemp@@QAE@XZ");
    p_gdtor = (Thiscall0_t)sim("??1EntityGroupTemp@@UAE@XZ");
    p_push  = (PushBack_t) sim("?push_back@?$ObservableContainer@PBVEntity@@$00@ast@@QAEXABQBVEntity@@@Z");
    p_id    = (GetID_t)    sim("?GetID@Entity@@QBEKXZ");
    p_ents  = (GetEnts_t)  sim("?GetEntities@EntityManager@@QAEAAV?$vector@PAVEntity@@V?$allocator@PAVEntity@@@_STL@@@_STL@@XZ");
    p_tctor = (TgtCtor_t)  sim("??0Target@@QAE@ABVVector3f@Math@@@Z");
    p_tdtor = (Thiscall0_t)sim("??1Target@@UAE@XZ");
    if (!p_type || !p_tpos || !p_begin || !p_end || !p_epos || !p_reset || !p_gctor || !p_gdtor || !p_push || !p_id || !p_ents || !p_tctor || !p_tdtor) { logmsg("SimEngine.dll functions not found - not hooking"); return 0; }
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
    logmsg("Reverse 4 start: hold %s and right-click the ground: back off there, %.0f m legs, next one %.0f m before the end - hook %s",
           g_key == VK_MENU ? "Alt" : g_key == VK_CONTROL ? "Ctrl" : "the key", g_step, g_lead, install() ? "installed" : "NOT installed");
    return 0;
}

static void load_ini(void) {
    wchar_t k[16]; GetPrivateProfileStringW(L"Reverse", L"Key", L"Ctrl", k, 16, g_ini);
    g_key = !lstrcmpiW(k, L"Alt") ? VK_MENU : !lstrcmpiW(k, L"Ctrl") ? VK_CONTROL : (int)wcstol(k, NULL, 0);
    if (g_key <= 0 || g_key > 254) g_key = VK_CONTROL;
    int st = GetPrivateProfileIntW(L"Reverse", L"StepMeters", 25, g_ini); if (st < 5) st = 5; if (st > 30) st = 30;
    g_step = (float)st;
    int ld = GetPrivateProfileIntW(L"Reverse", L"LeadMeters", 8, g_ini); if (ld < 0) ld = 0; if (ld > st - 3) ld = st - 3;
    g_lead = (float)ld;
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
