// MatchTimer.dll - match clock (MM:SS, real time) in Dawn of War II: Retribution 3.19 (Elite).
//
// The clock starts with the first simulation tick of a match and then runs on the PC's clock, so a lag
// ("waiting for players") doesn't stop it. Shown in every game, offline vs AI and replays included; OnlineOnly=1
// keeps it to live multiplayer (the session is online only there). Never in menus, on the loading screen or on
// the post-game screen (no sim ticks there).
// Sim ticks: World::Simulate is hooked through the DOW2.exe import table. Not World::SimulatePre / SimulatePost:
// DOW2.exe calls those through the import table only once per match, at its start and its end (seen with SimProbe).
// A new World object, or no tick for NewMatchGapSec, starts a new match. While loading, the game may run a few
// ticks and then wait for the other players: a pause within the first EARLY_TICKS ticks restarts the clock.
// Drawing: IDirect3DDevice9::EndScene through the device vtable + ID3DXFont, like CGNumbers (DXVK-safe);
// Reset is hooked too, so the font lets go of the device on alt-tab / resolution change.
// Read-only: nothing in the simulation changes, so no desync; only the player with the DLL sees the clock.
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#define COBJMACROS
#include <d3d9.h>

#define IAT_SIM         0x00F885E4   // World::Simulate (__thiscall, ecx = World), imported from SimEngine.dll, every sim tick
#define G_SESSION       0x01335678   // *(*G_SESSION + 4) & 0x558: the session is online (live MP), see docs/OBSERVER_LAG.md
#define ONLINE_MASK     0x558
#define EARLY_TICKS     16           // ~2 s of simulation: a pause before that is the loading screen, not a lag
#define EARLY_PAUSE_MS  1000

static wchar_t g_log[MAX_PATH], g_ini[MAX_PATH];
static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

// ---------- settings: MatchTimer.ini, re-read while the game runs when the file changes ----------
static int g_enabled = 1, g_x = 50, g_y = 8, g_align = 1, g_size = 22, g_onlineOnly = 0;
static DWORD g_color = 0xFFFFFFFF, g_outline = 0xFF000000, g_hideMs = 5000, g_newMatchMs = 60000;
static FILETIME g_iniTime;
static DWORD ini_color(const wchar_t *key, const wchar_t *def) {
    wchar_t s[16]; GetPrivateProfileStringW(L"MatchTimer", key, def, s, 16, g_ini);
    return 0xFF000000 | (wcstoul(s, NULL, 16) & 0xFFFFFF);
}
static int ini_int(const wchar_t *key, int def, int lo, int hi) {
    int v = GetPrivateProfileIntW(L"MatchTimer", key, def, g_ini);
    return v < lo ? lo : v > hi ? hi : v;
}
static void load_ini(void) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a)) g_iniTime = a.ftLastWriteTime;
    g_enabled    = ini_int(L"Enabled", 1, 0, 1);
    g_x          = ini_int(L"X", 50, 0, 100);
    g_y          = ini_int(L"Y", 8, 0, 100);
    g_align      = ini_int(L"Align", 1, 0, 2);
    g_size       = ini_int(L"Size", 22, 8, 200);
    g_onlineOnly = ini_int(L"OnlineOnly", 0, 0, 1);
    g_hideMs     = ini_int(L"HideAfterSec", 5, 1, 600) * 1000;
    g_newMatchMs = ini_int(L"NewMatchGapSec", 60, 5, 3600) * 1000;
    g_color      = ini_color(L"Color", L"FFFFFF");
    g_outline    = ini_color(L"Outline", L"000000");
}
static void reload_ini(DWORD now) {
    static DWORD last;
    if (now - last < 1000) return;
    last = now;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a) || !CompareFileTime(&a.ftLastWriteTime, &g_iniTime)) return;
    load_ini();
    logmsg("MatchTimer.ini re-read: Enabled=%d X=%d Y=%d Align=%d Size=%d OnlineOnly=%d", g_enabled, g_x, g_y, g_align, g_size, g_onlineOnly);
}

// ---------- match clock: sim ticks (sim thread) ----------
typedef void (__fastcall *ThisFn)(void *, void *);
static ThisFn o_sim;
static void *volatile g_world;                  // World of the current match
static volatile DWORD g_start, g_lastTick;      // GetTickCount() at the match start and at the last sim tick
static volatile LONG g_ticks;                   // sim ticks since g_start

static DWORD session_flags(void) {
    DWORD *g = (DWORD *)G_SESSION;
    if (IsBadReadPtr(g, 4) || !*g || IsBadReadPtr((void *)(*g + 4), 4)) return 0;
    return *(DWORD *)(*g + 4);
}
static void __fastcall hk_sim(void *self, void *edx) {
    DWORD now = GetTickCount(), gap = now - g_lastTick;
    if (self != g_world || !g_lastTick || gap > g_newMatchMs) {
        logmsg("match start: World %p (previous %p, last tick %lu s ago), session flags %lX",
               self, g_world, g_lastTick ? gap / 1000 : 0, session_flags());
        g_world = self; g_start = now; g_ticks = 0;
    } else if (g_ticks < EARLY_TICKS && gap > EARLY_PAUSE_MS) {
        logmsg("clock restarted: %lu ms pause after tick %ld (loading screen)", gap, g_ticks);
        g_start = now; g_ticks = 0;
    } else if (gap > 3000)
        logmsg("sim stood still for %lu s (lag?), the clock kept running", gap / 1000);
    g_lastTick = now; g_ticks++;
    o_sim(self, edx);
}
static int clock_visible(DWORD now) {
    if (!g_lastTick || g_ticks < EARLY_TICKS) return 0;            // no match yet, or still on the loading screen
    if (now - g_lastTick > g_hideMs) return 0;                      // sim stopped: menu, post-game screen or a long lag
    return !g_onlineOnly || (session_flags() & ONLINE_MASK);        // replays and offline games have no online session
}

// slot already hooked by another DLL (e.g. FrameProbe) - chain onto it if it points to executable code (as in FrameProbe)
static int iat_hook(DWORD slot, const char *dll, void *fn, void **orig) {
    HMODULE m = GetModuleHandleA(dll); MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || IsBadReadPtr((void *)slot, 4)) return 0;
    DWORD cur = *(DWORD *)slot;
    if (cur < (DWORD)mi.lpBaseOfDll || cur >= (DWORD)mi.lpBaseOfDll + mi.SizeOfImage) {
        MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery((void *)cur, &mb, sizeof mb) || !(mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
        HMODULE exe = GetModuleHandleA(NULL); MODULEINFO me;               // but only inside the game itself (DOW2.exe)
        if (!GetModuleInformation(GetCurrentProcess(), exe, &me, sizeof me) || slot < (DWORD)me.lpBaseOfDll || slot >= (DWORD)me.lpBaseOfDll + me.SizeOfImage) return 0;
        char nm[MAX_PATH]; GetModuleFileNameA(exe, nm, MAX_PATH); if (!strstr(nm, "DOW2") && !strstr(nm, "dow2")) return 0;
    }
    DWORD old; if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return 0;
    *orig = (void *)cur; *(DWORD *)slot = (DWORD)fn;
    VirtualProtect((void *)slot, 4, old, &old);
    return 1;
}

// ---------- minimal ID3DXFont / ID3DXSprite (no d3dx9 headers needed; only the methods we call are typed) ----------
typedef struct ID3DXSprite ID3DXSprite;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(ID3DXSprite *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(ID3DXSprite *);
    ULONG   (WINAPI *Release)(ID3DXSprite *);
    void *GetDevice, *GetTransform, *SetTransform, *SetWorldViewRH, *SetWorldViewLH;
    HRESULT (WINAPI *Begin)(ID3DXSprite *, DWORD flags);
    void *Draw, *Flush;
    HRESULT (WINAPI *End)(ID3DXSprite *);
    HRESULT (WINAPI *OnLostDevice)(ID3DXSprite *);
    HRESULT (WINAPI *OnResetDevice)(ID3DXSprite *);
} ID3DXSpriteVtbl;
struct ID3DXSprite { ID3DXSpriteVtbl *lpVtbl; };
typedef struct ID3DXFont ID3DXFont;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(ID3DXFont *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(ID3DXFont *);
    ULONG   (WINAPI *Release)(ID3DXFont *);
    void *GetDevice, *GetDescA, *GetDescW, *GetTextMetricsA, *GetTextMetricsW, *GetDC, *GetGlyphData;
    void *PreloadCharacters, *PreloadGlyphs, *PreloadTextA, *PreloadTextW;
    INT     (WINAPI *DrawTextA)(ID3DXFont *, ID3DXSprite *, const char *, INT, RECT *, DWORD format, D3DCOLOR);
    void *DrawTextW;
    HRESULT (WINAPI *OnLostDevice)(ID3DXFont *);
    HRESULT (WINAPI *OnResetDevice)(ID3DXFont *);
} ID3DXFontVtbl;
struct ID3DXFont { ID3DXFontVtbl *lpVtbl; };
typedef HRESULT (WINAPI *D3DXCreateFontA_t)(IDirect3DDevice9 *, INT height, UINT width, UINT weight, UINT miplevels, BOOL italic,
    DWORD charset, DWORD prec, DWORD quality, DWORD pitchfam, const char *face, ID3DXFont **out);
typedef HRESULT (WINAPI *D3DXCreateSprite_t)(IDirect3DDevice9 *, ID3DXSprite **out);
static D3DXCreateFontA_t p_D3DXCreateFontA;
static D3DXCreateSprite_t p_D3DXCreateSprite;
#define D3DXSPRITE_ALPHABLEND   0x10
#define D3DXSPRITE_SORT_TEXTURE 0x20

// ---------- drawing (render thread) ----------
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9 *);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static EndScene_t o_EndScene; static Reset_t o_Reset;
static ID3DXFont *g_font; static ID3DXSprite *g_sprite;
static IDirect3DDevice9 *g_fontDev; static int g_fontPx, g_lost;
static int g_shown; static DWORD g_shownAt; static int g_diagDone;
static volatile LONG g_totalCalls; static LONG g_calls, g_bbCalls;   // EndScene calls, and how many of them went into the back buffer

static int ensure_font(IDirect3DDevice9 *dev, int px) {
    if (g_fontDev == dev && g_fontPx == px) return g_font && g_sprite;  // made already (or failed: no retry every frame)
    if (g_font) { g_font->lpVtbl->Release(g_font); g_font = NULL; }
    if (g_sprite) { g_sprite->lpVtbl->Release(g_sprite); g_sprite = NULL; }
    g_fontDev = dev; g_fontPx = px;
    HRESULT hf = p_D3DXCreateFontA(dev, px, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial", &g_font);
    HRESULT hs = p_D3DXCreateSprite(dev, &g_sprite);
    logmsg("font %dpx -> hr=0x%08lx, sprite -> hr=0x%08lx", px, (unsigned long)hf, (unsigned long)hs);
    if (FAILED(hf)) g_font = NULL;
    if (FAILED(hs)) g_sprite = NULL;
    return g_font && g_sprite;
}
// only the frame itself gets the clock, not passes into textures (shadows, minimap...)
static int on_back_buffer(IDirect3DDevice9 *dev, int *w, int *h) {
    IDirect3DSurface9 *bb = NULL, *rt = NULL; D3DSURFACE_DESC d; int ok = 0;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) && rt) {
            if (rt == bb && SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d))) { *w = d.Width; *h = d.Height; ok = 1; }
            IDirect3DSurface9_Release(rt);
        }
        IDirect3DSurface9_Release(bb);
    }
    return ok;
}
static void draw(IDirect3DDevice9 *dev, DWORD now) {
    int W, H;
    if (!on_back_buffer(dev, &W, &H)) return;
    g_bbCalls++;
    int px = g_size * H / 1080; if (px < 10) px = 10;               // Size is for 1080p, scaled with the screen height
    if (g_lost || !ensure_font(dev, px)) return;
    DWORD ms = now - g_start, s = (LONG)ms < 0 ? 0 : ms / 1000; char t[16];   // the sim thread may have just moved g_start

    if (s >= 3600) wsprintfA(t, "%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
    else wsprintfA(t, "%02lu:%02lu", s / 60, s % 60);
    int x = W * g_x / 100, y = H * g_y / 100, o = px >= 28 ? 2 : 1;
    RECT r = { g_align == 0 ? x : x - W, y, g_align == 2 ? x : x + W, y + 2 * px };
    DWORD fmt = DT_SINGLELINE | DT_NOCLIP | (g_align == 0 ? DT_LEFT : g_align == 2 ? DT_RIGHT : DT_CENTER);
    D3DVIEWPORT9 vp, full = { 0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f };
    int haveVp = SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &vp));
    IDirect3DDevice9_SetViewport(dev, &full);                        // the game may have left a smaller viewport
    if (SUCCEEDED(g_sprite->lpVtbl->Begin(g_sprite, D3DXSPRITE_ALPHABLEND | D3DXSPRITE_SORT_TEXTURE))) {
        static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        for (int i = 0; i < 8; i++) {                                // outline, so the clock reads on any terrain
            RECT q = { r.left + dx[i] * o, r.top + dy[i] * o, r.right + dx[i] * o, r.bottom + dy[i] * o };
            g_font->lpVtbl->DrawTextA(g_font, g_sprite, t, -1, &q, fmt, g_outline);
        }
        g_font->lpVtbl->DrawTextA(g_font, g_sprite, t, -1, &r, fmt, g_color);
        g_sprite->lpVtbl->End(g_sprite);
    }
    if (haveVp) IDirect3DDevice9_SetViewport(dev, &vp);
}
static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *dev) {
    DWORD now = GetTickCount();
    InterlockedIncrement(&g_totalCalls); g_calls++;
    reload_ini(now);
    int vis = g_enabled && clock_visible(now);
    if (vis != g_shown) {
        g_shown = vis;
        DWORD run = g_lastTick - g_start;
        if (vis) { logmsg("clock shown (session flags %lX)", session_flags()); g_shownAt = now; g_calls = g_bbCalls = 0; g_diagDone = 0; }
        else logmsg("clock hidden: last sim tick %lu s ago, match %lu:%02lu, %ld ticks (%.1f per second)",
                    (now - g_lastTick) / 1000, run / 60000, run / 1000 % 60, g_ticks, run ? g_ticks * 1000.0 / run : 0.0);
    }
    if (vis) {
        draw(dev, now);
        if (!g_diagDone && now - g_shownAt > 10000) {
            g_diagDone = 1;
            logmsg("EndScene in 10 s: %ld calls, %ld into the back buffer%s", g_calls, g_bbCalls,
                   g_bbCalls ? "" : " - the clock is not drawn, tell the author");
        }
    }
    return o_EndScene(dev);
}
// ID3DXFont/ID3DXSprite hold state blocks and video memory: they must let go before Reset or Reset fails
static HRESULT WINAPI hk_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    if (dev == g_fontDev && !g_lost && (g_font || g_sprite)) {
        if (g_font) g_font->lpVtbl->OnLostDevice(g_font);
        if (g_sprite) g_sprite->lpVtbl->OnLostDevice(g_sprite);
        g_lost = 1;
    }
    HRESULT hr = o_Reset(dev, pp);
    if (g_lost && SUCCEEDED(hr)) {
        if (g_font) g_font->lpVtbl->OnResetDevice(g_font);
        if (g_sprite) g_sprite->lpVtbl->OnResetDevice(g_sprite);
        g_lost = 0;
    }
    logmsg("device Reset -> hr=0x%08lx", (unsigned long)hr);
    return hr;
}

// patch one COM vtable slot, return the previous pointer
static void *vt_hook(void **vtbl, int index, void *fn) {
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    void *prev = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    return prev;
}
// create a throwaway device to reach the device vtable (shared with the game's device), then hook it
static int install_d3d_hooks(void) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    IDirect3D9 *(WINAPI *pCreate)(UINT) = d3d9 ? (void *)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
    if (!pCreate) { logmsg("no Direct3DCreate9"); return 0; }
    IDirect3D9 *d3d = pCreate(D3D_SDK_VERSION);
    if (!d3d) { logmsg("Direct3DCreate9 failed"); return 0; }
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mtprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("mtprobe", "mt", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hwnd;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (FAILED(hr) || !dev) {
        logmsg("temp CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        IDirect3D9_Release(d3d); if (hwnd) DestroyWindow(hwnd); return 0;
    }
    void **vtbl = *(void ***)dev;
    o_Reset    = (Reset_t)    vt_hook(vtbl, 16, (void *)hk_Reset);
    o_EndScene = (EndScene_t) vt_hook(vtbl, 42, (void *)hk_EndScene);
    logmsg("vtable hooked: Reset16=%p EndScene42=%p", (void *)o_Reset, (void *)o_EndScene);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    if (hwnd) DestroyWindow(hwnd);
    return 1;
}

static DWORD WINAPI worker(LPVOID _) {
    const char *vers[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "d3dx9_38.dll", "d3dx9_35.dll", "d3dx9_34.dll", "d3dx9_31.dll", "d3dx9_30.dll", NULL };
    for (int i = 0; vers[i]; i++) {
        HMODULE h = LoadLibraryA(vers[i]); if (!h) continue;
        p_D3DXCreateFontA = (D3DXCreateFontA_t)GetProcAddress(h, "D3DXCreateFontA");
        p_D3DXCreateSprite = (D3DXCreateSprite_t)GetProcAddress(h, "D3DXCreateSprite");
        if (p_D3DXCreateFontA && p_D3DXCreateSprite) { logmsg("using %s", vers[i]); break; }
    }
    if (!p_D3DXCreateFontA || !p_D3DXCreateSprite) { logmsg("no d3dx9_*.dll with D3DXCreateFontA - the clock can't be drawn"); return 0; }
    int sim = 0;
    for (int i = 0; i < 120; i++) {
        if (!sim && (sim = iat_hook(IAT_SIM, "SimEngine.dll", hk_sim, (void **)&o_sim))) logmsg("sim tick hook installed");
        if (sim && GetModuleHandleA("d3d9.dll") && install_d3d_hooks()) {
            logmsg("hooks installed after %d s", i);
            Sleep(60000);                                            // menus draw every frame too, so EndScene must have run by now
            if (!g_totalCalls) logmsg("EndScene not called in a minute: the game's device doesn't use the hooked vtable, nothing will be drawn");
            return 0;
        }
        Sleep(1000);
    }
    if (sim) logmsg("gave up waiting for d3d9.dll");
    else logmsg("World::Simulate import not found at %08X - not DOW2.exe 3.19? The clock stays off", IAT_SIM);
    return 0;
}

// one copy per game process: a patch can come both from Elite_DLLs (launcher) and from dow2-patches\ (winmm loader)
static int already_loaded(const char *name) {
    char n[96]; wsprintfA(n, "Local\\dow2patch_%s_%lu", name, GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, n);
    return m && GetLastError() == ERROR_ALREADY_EXISTS;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wcscpy(g_ini, g_log); wcscat(g_ini, L"MatchTimer.ini"); wcscat(g_log, L"MatchTimer.log");
    load_ini();
    logmsg("MatchTimer v1");
    if (already_loaded("MatchTimer")) { logmsg("another MatchTimer copy is already active in this game, this one stays idle"); return TRUE; }
    if (!g_enabled) { logmsg("disabled in MatchTimer.ini"); return TRUE; }
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}
