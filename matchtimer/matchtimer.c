// MatchTimer.dll - match clock (MM:SS of game time) in Dawn of War II: Retribution 3.19 (Elite).
//
// Game time: the clock runs while the simulation ticks and stands still while it doesn't (pause, offline game
// minimized, "waiting for players"), so every player of a match sees the same time, the replay too. A minimized
// online match keeps ticking, so its clock keeps running. Shown in every game, offline vs AI and replays included;
// OnlineOnly=1 keeps it to live multiplayer (the session is online only there).
// Hooks through the DOW2.exe import table (SimEngine.dll), seen with SimProbe:
//   World::SimulatePre  - once per match, at its start (still on the loading screen): the clock is reset;
//   World::Simulate     - every sim tick: the time between two ticks is added, a gap over PAUSE_MS is not;
//   World::SimulatePost - once per match, at its end (victory, defeat or leaving through the menu): clock hidden.
// Drawing: IDirect3DDevice9::EndScene through the device vtable, like CGNumbers (DXVK-safe). The digits are in the
// game's own Caslon Antique New, read from its files at start (common/gamefont.h), else a Windows font (ID3DXFont);
// Reset is hooked too, so the font lets go of the device on alt-tab / resolution change.
// Read-only: nothing in the simulation changes, so no desync; only the player with the DLL sees the clock.
//
// Built two ways: MatchTimer.dll on its own, or with -DMT_EMBED inside LobbyStats.dll (one device hook for both: the
// clock, the main menu tooltip and the ratings on the loading screen; settings in LobbyStats.ini, section [MatchTimer]).
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#define COBJMACROS
#include <d3d9.h>

#define IAT_SIM_PRE     0x00F88154   // World::SimulatePre  (__thiscall, ecx = World), match start
#define IAT_SIM         0x00F885E4   // World::Simulate     (__thiscall, ecx = World), every sim tick (~8 per second)
#define IAT_SIM_POST    0x00F88100   // World::SimulatePost (__thiscall, ecx = World), match end
#define G_SESSION       0x01335678   // *(*G_SESSION + 4) & 0x558: the session is online (live MP), see docs/OBSERVER_LAG.md
#define ONLINE_MASK     0x558
#define PAUSE_MS        1000         // a longer gap between two ticks is a pause or a lag: not game time

static wchar_t g_log[MAX_PATH], g_ini[MAX_PATH];
static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

// ---------- settings: MatchTimer.ini, re-read while the game runs when the file changes ----------
static int g_enabled = 1, g_x = 50, g_y = 8, g_align = 1, g_size = 20, g_onlineOnly = 0;
static DWORD g_color = 0xFFF0EEE4, g_outline = 0;
// Style=1: left of the Menu button. The button sits in the top right corner and scales with the screen height
// (the same in every locale), so the clock is placed from the right edge in 1/1000 of the screen height.
// Style=2: the same place, in a box drawn like the game's own (the "Fog of War Off" box of the replay panel).
static int g_style = 2, g_bold = 0, g_shadow = 1, g_menuX = 131, g_menuY = 18;
// Anchor=1 (Style=1/2): right of the victory points bar instead; VpX = the left edge from the screen centre.
static int g_anchor = 0, g_vpX = 126;      // the look siritos chose (28.10): a box left of the Menu button
// Anchor=2: above the minimap (its frame starts 221/1000 H above the bottom edge): left edge MapX, middle MapY above
// the bottom edge, in 1/1000 of the screen height
static int g_mapX = 7, g_mapY = 239;
// opacity of the outline in Style=1/2, 0..255: 144 = a soft halo, 255 = a crisp outline like the campaign objective timers
static int g_outlineAlpha = 160;
static char g_face[64] = "Arial";
// the game's own font, read from its files (gamefont.h); Font= naming it draws the digits with it
#define GAME_FONT "Caslon Antique New"
static int g_gfWidth = 100;                        // GameFontWidth=: horizontal scale of the game font's digits, percent
static FILETIME g_iniTime;
static DWORD ini_color(const wchar_t *key, const wchar_t *def) {
    wchar_t s[16]; GetPrivateProfileStringW(L"MatchTimer", key, def, s, 16, g_ini);
    return *s ? 0xFF000000 | (wcstoul(s, NULL, 16) & 0xFFFFFF) : 0;  // empty = none (alpha 0)
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
    g_size       = ini_int(L"Size", 20, 8, 200);
    g_onlineOnly = ini_int(L"OnlineOnly", 0, 0, 1);
    g_color      = ini_color(L"Color", L"F0EEE4");
    g_outline    = ini_color(L"Outline", L"");
    g_style      = ini_int(L"Style", 2, 0, 2);
    g_bold       = ini_int(L"Bold", 0, 0, 1);
    g_shadow     = ini_int(L"Shadow", 1, 0, 1);
    g_menuX      = ini_int(L"MenuX", 131, 0, 2000);
    g_menuY      = ini_int(L"MenuY", 18, 0, 1000);
    g_anchor     = ini_int(L"Anchor", 0, 0, 2);
    g_mapX       = ini_int(L"MapX", 7, 0, 2000);
    g_mapY       = ini_int(L"MapY", 239, 0, 1000);
    g_vpX        = ini_int(L"VpX", 126, -1000, 2000);
    g_outlineAlpha = ini_int(L"OutlineAlpha", 160, 0, 255);
    g_gfWidth    = ini_int(L"GameFontWidth", 100, 50, 200);
    wchar_t f[64]; GetPrivateProfileStringW(L"MatchTimer", L"Font", L"Arial", f, 64, g_ini);
    WideCharToMultiByte(CP_ACP, 0, f, -1, g_face, sizeof g_face, NULL, NULL);
}
static void reload_ini(DWORD now) {
    static DWORD last;
    if (now - last < 1000) return;
    last = now;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a) || !CompareFileTime(&a.ftLastWriteTime, &g_iniTime)) return;
    load_ini();
    logmsg("MatchTimer.ini re-read: Enabled=%d Style=%d Font=%s Size=%d MenuX=%d MenuY=%d X=%d Y=%d Align=%d OnlineOnly=%d", g_enabled,
           g_style, g_face, g_size, g_menuX, g_menuY, g_x, g_y, g_align, g_onlineOnly);
}

// ---------- match clock: sim ticks (sim thread) ----------
typedef void (__fastcall *ThisFn)(void *, void *);
static ThisFn o_pre, o_sim, o_post;
static void *volatile g_world;                  // World of the current match
static volatile int g_inMatch;                  // between SimulatePre and SimulatePost
static volatile DWORD g_play, g_lastTick;       // game time of the match in ms, GetTickCount() at the last sim tick
static volatile LONG g_ticks;                   // sim ticks in this match

static DWORD session_flags(void) {
    DWORD *g = (DWORD *)G_SESSION;
    if (IsBadReadPtr(g, 4) || !*g || IsBadReadPtr((void *)(*g + 4), 4)) return 0;
    return *(DWORD *)(*g + 4);
}
static void match_start(void *world, const char *why) {
    logmsg("match start (%s): World %p, previous %p, session flags %lX", why, world, g_world, session_flags());
    g_world = world; g_play = 0; g_lastTick = 0; g_ticks = 0; g_inMatch = 1;
}
static void __fastcall hk_pre(void *self, void *edx) {
    match_start(self, "SimulatePre");
    o_pre(self, edx);
}
static void __fastcall hk_post(void *self, void *edx) {
    logmsg("match end: %lu:%02lu of game time, %ld ticks", g_play / 60000, g_play / 1000 % 60, g_ticks);
    g_inMatch = 0;
    o_post(self, edx);
}
static void __fastcall hk_sim(void *self, void *edx) {
    DWORD now = GetTickCount(), gap = now - g_lastTick;
    if (self != g_world) match_start(self, "first tick of a new World");    // e.g. the DLL came in mid-match
    else if (g_lastTick && gap <= PAUSE_MS) g_play += gap;
    else if (g_lastTick && gap > 3000) logmsg("sim stood still for %lu s (pause or lag), the clock waited", gap / 1000);
    g_lastTick = now; g_ticks++;
    o_sim(self, edx);
}
static int clock_visible(void) {
    if (!g_inMatch || !g_ticks) return 0;                            // menus, loading screen, post-game screen
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
    HRESULT (WINAPI *Draw)(ID3DXSprite *, IDirect3DTexture9 *, const RECT *src, const float *center, const float *pos, D3DCOLOR);
    void *Flush;
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
#include "../common/gamefont.h"
static GameFont g_gf = { "0123456789:" };                     // the clock's digits in the game's Caslon Antique New

// ---------- drawing (render thread) ----------
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9 *);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static EndScene_t o_EndScene; static Reset_t o_Reset;
static ID3DXFont *g_font; static ID3DXSprite *g_sprite;
static IDirect3DDevice9 *g_fontDev; static int g_fontPx, g_lost;
static int g_shown; static DWORD g_shownAt; static int g_diagDone;
static volatile LONG g_totalCalls; static LONG g_calls, g_bbCalls;   // EndScene calls, and how many of them went into the back buffer

static int ensure_font(IDirect3DDevice9 *dev, int px) {
    static char face[64]; static int weight;
    // Style 0 keeps the old look: bold Arial
    const char *want = !g_style ? "Arial" : !strcmp(g_face, GAME_FONT) ? "Book Antiqua" : g_face;   // the game font's fallback
    int w = g_style && !g_bold ? FW_NORMAL : FW_BOLD;
    if (g_fontDev == dev && g_fontPx == px && weight == w && !strcmp(face, want))
        return g_font && g_sprite;                                       // made already (or failed: no retry every frame)
    if (g_font) { g_font->lpVtbl->Release(g_font); g_font = NULL; }
    if (g_sprite) { g_sprite->lpVtbl->Release(g_sprite); g_sprite = NULL; }
    g_fontDev = dev; g_fontPx = px; weight = w; lstrcpynA(face, want, sizeof face);
    HRESULT hf = p_D3DXCreateFontA(dev, px, 0, w, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face, &g_font);
    HRESULT hs = p_D3DXCreateSprite(dev, &g_sprite);
    logmsg("font %s %dpx -> hr=0x%08lx, sprite -> hr=0x%08lx", face, px, (unsigned long)hf, (unsigned long)hs);
    if (FAILED(hf)) g_font = NULL;
    if (FAILED(hs)) g_sprite = NULL;
    return g_font && g_sprite;
}
// ---------- Style=2 box: a texture painted once per size, colours taken from the replay panel at 1440p ----------
// The game's box is 37 px high there: a dark frame with cut corners, a thin steel line inside it, slate-blue fill.
static IDirect3DTexture9 *g_box; static IDirect3DDevice9 *g_boxDev; static int g_boxW, g_boxH;
static DWORD mix(DWORD a, DWORD b, float t) {                        // RGB lerp
    DWORD r = 0;
    for (int s = 0; s < 24; s += 8) r |= (DWORD)(((a >> s) & 255) + (((int)((b >> s) & 255) - (int)((a >> s) & 255)) * t)) << s;
    return r;
}
static DWORD box_pixel(int x, int y, int w, int h) {
    float k = h / 37.0f, c = 5 * k, px = x + 0.5f, py = y + 0.5f, R = 0.7071f;
    float d = px; if (py < d) d = py; if (w - px < d) d = w - px; if (h - py < d) d = h - py;      // distance to the edge
    float e[4] = { (px + py - c) * R, (w - px + py - c) * R, (px + h - py - c) * R, (w - px + h - py - c) * R };
    for (int i = 0; i < 4; i++) if (e[i] < d) d = e[i];                                        // cut corners
    if (d <= 0) return 0;
    DWORD rgb;
    if (d < 1) rgb = 0x5A6A6C;                                         // light rim against the scene
    else if (d < 3.5f * k) rgb = 0x0E1D29;                             // dark frame
    else if (d < 4.5f * k) rgb = 0x48606E;                             // steel line
    else {                                                             // fill, a little lighter at the top, with grain
        rgb = mix(0x2D4556, 0x22384A, py / h);
        unsigned n = (unsigned)(x * 73856093u ^ y * 19349663u); n = (n ^ (n >> 13)) * 1274126177u;
        int g = (int)((n >> 24) % 7) - 3;
        DWORD o = 0;
        for (int s = 0; s < 24; s += 8) { int v = (int)((rgb >> s) & 255) + g; o |= (DWORD)(v < 0 ? 0 : v > 255 ? 255 : v) << s; }
        rgb = o;
    }
    DWORD a = d < 1 ? (DWORD)(d * 255) : 255;                          // soft outer edge
    return a << 24 | rgb;
}
static int ensure_box(IDirect3DDevice9 *dev, int w, int h) {
    if (g_box && g_boxDev == dev && g_boxW == w && g_boxH == h) return 1;
    if (g_box) { IDirect3DTexture9_Release(g_box); g_box = NULL; }
    g_boxDev = dev; g_boxW = w; g_boxH = h;
    // MANAGED: survives a device Reset on its own
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_box, NULL))) {
        logmsg("box texture %dx%d failed", w, h); g_box = NULL; return 0;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_box, 0, &lr, NULL, 0))) { IDirect3DTexture9_Release(g_box); g_box = NULL; return 0; }
    for (int y = 0; y < h; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) row[x] = box_pixel(x, y, w, h);
    }
    IDirect3DTexture9_UnlockRect(g_box, 0);
    return 1;
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
    int game = g_style && !strcmp(g_face, GAME_FONT) && (g_gf.width = g_gfWidth, gf_ready(&g_gf, dev, px));   // the game's own digits (gamefont.h)
    DWORD s = g_play / 1000; char t[16];

    if (s >= 3600) wsprintfA(t, "%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
    else wsprintfA(t, "%02lu:%02lu", s / 60, s % 60);
    int o = game ? 1 : px >= 28 ? 2 : 1;                             // the game font: a thin outline, as on the campaign timers
    RECT r, box = {0}; DWORD fmt;
    if (g_style == 2) {                                              // box of the Menu button's height, next to it or to the VP bar
        char z[16]; int i;                                           // width from all-zero digits, so the box doesn't twitch
        for (i = 0; t[i]; i++) z[i] = t[i] == ':' ? ':' : '0';
        z[i] = 0;
        RECT m = {0, 0, 0, 0};
        if (game) m.right = gf_width(&g_gf, z); else g_font->lpVtbl->DrawTextA(g_font, NULL, z, -1, &m, DT_SINGLELINE | DT_CALCRECT, 0);
        int bh = 26 * H / 1000, pad = bh * 4 / 10, bw = m.right - m.left + 2 * pad;
        int top = (g_anchor == 2 ? H - g_mapY * H / 1000 : g_menuY * H / 1000) - bh / 2;
        if (g_anchor) {                                              // right of the VP bar (centred) or above the minimap
            int left = g_anchor == 2 ? g_mapX * H / 1000 : W / 2 + g_vpX * H / 1000;
            SetRect(&box, left, top, left + bw, top + bh);
        } else {
            int right = W - g_menuX * H / 1000;
            SetRect(&box, right - bw, top, right, top + bh);
        }
        r = box;
        fmt = DT_SINGLELINE | DT_NOCLIP | DT_CENTER | DT_VCENTER;
        if (!ensure_box(dev, bw, bh)) box.right = box.left;          // no texture: text only
    } else if (g_style) {                                            // text only, next to the Menu button or the VP bar
        int cy = g_anchor == 2 ? H - g_mapY * H / 1000 : g_menuY * H / 1000;
        if (g_anchor) { int left = g_anchor == 2 ? g_mapX * H / 1000 : W / 2 + g_vpX * H / 1000; SetRect(&r, left, cy - px, left + W, cy + px); }
        else { int right = W - g_menuX * H / 1000; SetRect(&r, right - W, cy - px, right, cy + px); }
        fmt = DT_SINGLELINE | DT_NOCLIP | (g_anchor ? DT_LEFT : DT_RIGHT) | DT_VCENTER;
    } else {
        int x = W * g_x / 100, y = H * g_y / 100;
        SetRect(&r, g_align == 0 ? x : x - W, y, g_align == 2 ? x : x + W, y + 2 * px);
        fmt = DT_SINGLELINE | DT_NOCLIP | (g_align == 0 ? DT_LEFT : g_align == 2 ? DT_RIGHT : DT_CENTER);
    }
    D3DVIEWPORT9 vp, full = { 0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f };
    int haveVp = SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &vp));
    IDirect3DDevice9_SetViewport(dev, &full);                        // the game may have left a smaller viewport
    if (SUCCEEDED(g_sprite->lpVtbl->Begin(g_sprite, D3DXSPRITE_ALPHABLEND))) {   // no sorting: box first, then the text
        if (box.right > box.left) {
            float pos[3] = { (float)box.left, (float)box.top, 0.0f };
            g_sprite->lpVtbl->Draw(g_sprite, g_box, NULL, NULL, pos, 0xFFFFFFFF);
        }
        // optional outline (Outline= empty: none); in Style=1/2 OutlineAlpha sets how solid it is
        DWORD halo = g_style ? (g_outline & 0xFFFFFF) | (DWORD)g_outlineAlpha << 24 : g_outline;
        static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        for (int i = 0; (g_outline >> 24) && i < 8; i++) {
            RECT q = { r.left + dx[i] * o, r.top + dy[i] * o, r.right + dx[i] * o, r.bottom + dy[i] * o };
            if (game) gf_draw(&g_gf, g_sprite, t, &q, fmt, halo); else g_font->lpVtbl->DrawTextA(g_font, g_sprite, t, -1, &q, fmt, halo);
        }
        if (g_shadow) {
            // the game's own text shadow (replay timer, campaign objectives: DropShadowFilter distance 2, angle 70 deg,
            // blur 1, strength 0.75, black): 2 px down and a bit right at 1080p, a light blur around it
            int sx = H >= 1620 ? 2 : 1, sy = 2 * H / 1080 > 2 ? 2 * H / 1080 : 2;
            static const int bx[4] = {-1, 1, 0, 0}, by[4] = {0, 0, -1, 1};
            for (int i = 0; i < 5; i++) {
                RECT q = { r.left + sx, r.top + sy, r.right + sx, r.bottom + sy };
                DWORD c = 0xC0000000;
                if (i < 4) { OffsetRect(&q, bx[i], by[i]); c = 0x30000000; }
                if (game) gf_draw(&g_gf, g_sprite, t, &q, fmt, c); else g_font->lpVtbl->DrawTextA(g_font, g_sprite, t, -1, &q, fmt, c);
            }
        }
        if (game) gf_draw(&g_gf, g_sprite, t, &r, fmt, g_color); else g_font->lpVtbl->DrawTextA(g_font, g_sprite, t, -1, &r, fmt, g_color);
        g_sprite->lpVtbl->End(g_sprite);
    }
    if (haveVp) IDirect3DDevice9_SetViewport(dev, &vp);
}
#ifdef MT_EMBED
// the host DLL (LobbyStats) draws its own things through the same hook: EndScene, and before/after a device Reset
void (*mt_extra_draw)(IDirect3DDevice9 *);
void (*mt_extra_lost)(IDirect3DDevice9 *);
void (*mt_extra_reset)(IDirect3DDevice9 *, HRESULT);
static int g_clockOff;                          // a stand-alone MatchTimer.dll draws the clock already
int mt_loading(void) { return g_inMatch && !g_ticks; }   // SimulatePre came, no sim tick yet: the loading screen
int mt_online(void) { return (session_flags() & ONLINE_MASK) != 0; }
int mt_ticking(void) { return g_inMatch && g_ticks; }                  // the match runs (past its loading screen)
#endif
static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *dev) {
    DWORD now = GetTickCount();
#ifdef MT_EMBED
    if (mt_extra_draw) mt_extra_draw(dev);
    if (g_clockOff) return o_EndScene(dev);
#endif
    InterlockedIncrement(&g_totalCalls); g_calls++;
    reload_ini(now);
    int vis = g_enabled && clock_visible();
    if (vis != g_shown) {
        g_shown = vis;
        DWORD run = g_play;
        if (vis) { logmsg("clock shown (session flags %lX)", session_flags()); g_shownAt = now; g_calls = g_bbCalls = 0; g_diagDone = 0; }
        else logmsg("clock hidden: match %lu:%02lu of game time, %ld ticks (%.1f per second)",
                    run / 60000, run / 1000 % 60, g_ticks, run ? g_ticks * 1000.0 / run : 0.0);
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
#ifdef MT_EMBED
    if (mt_extra_lost) mt_extra_lost(dev);
#endif
    HRESULT hr = o_Reset(dev, pp);
#ifdef MT_EMBED
    if (mt_extra_reset) mt_extra_reset(dev, hr);
#endif
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
// The device: no throwaway HAL device (v2 made one every second until it worked - while the game held a fullscreen device
// that broke alt-tab / minimise and could hang the PC). v3 waits for the game itself: Spooge.dll imports Direct3DCreate9,
// we take that import, then IDirect3D9::CreateDevice (vtable slot 16), and hook EndScene/Reset of the device the game makes.
// Only if the game had made its device before we came (late load), one NULLREF device (no video card involved) shows
// the vtable - one try, never a loop.
typedef IDirect3D9 *(WINAPI *Create9_t)(UINT);
typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static Create9_t o_Create9; static CreateDevice_t o_CreateDevice; static volatile LONG g_devHooked, g_d3dHooked;
static void hook_device(IDirect3DDevice9 *dev) {
    if (!dev || InterlockedExchange(&g_devHooked, 1)) return;
    void **vtbl = *(void ***)dev;
    o_Reset    = (Reset_t)    vt_hook(vtbl, 16, (void *)hk_Reset);
    o_EndScene = (EndScene_t) vt_hook(vtbl, 42, (void *)hk_EndScene);
    logmsg("device vtable hooked: Reset16=%p EndScene42=%p", (void *)o_Reset, (void *)o_EndScene);
}
static HRESULT WINAPI hk_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    HRESULT hr = o_CreateDevice(d3d, ad, t, w, f, pp, out);
    logmsg("the game created its device: type %d hr=0x%08lx", (int)t, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out && t == D3DDEVTYPE_HAL) hook_device(*out);
    return hr;
}
static IDirect3D9 *WINAPI hk_Create9(UINT v) {
    IDirect3D9 *d3d = o_Create9(v);
    if (d3d && !InterlockedExchange(&g_d3dHooked, 1)) {
        o_CreateDevice = (CreateDevice_t) vt_hook(*(void ***)d3d, 16, (void *)hk_CreateDevice);
        logmsg("IDirect3D9::CreateDevice hooked");
    }
    return d3d;
}
// the import slot of Direct3DCreate9 in every loaded module (Spooge.dll for DOW2 3.19)
static int hook_create9(void) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    void *real = d3d9 ? (void *)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
    if (!real) return 0;
    o_Create9 = (Create9_t)real;
    HMODULE mods[512]; DWORD need = 0; int n = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return 0;
    for (DWORD m = 0; m < need / sizeof(HMODULE) && m < 512; m++) {
        unsigned char *base = (unsigned char *)mods[m]; IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)base;
        if (IsBadReadPtr(dh, sizeof *dh) || dh->e_magic != IMAGE_DOS_SIGNATURE) continue;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dh->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) continue;
        for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
            if (lstrcmpiA((const char *)(base + imp->Name), "d3d9.dll")) continue;
            for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk); t->u1.Function; t++)
                if ((void *)t->u1.Function == real) {
                    DWORD o; VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &o); t->u1.Function = (DWORD)hk_Create9; VirtualProtect(&t->u1.Function, 4, o, &o); n++;
                }
        }
    }
    return n;
}
// fallback for a late load: one NULLREF device (no video card, works while the game is fullscreen or minimised)
static int hook_by_nullref(void) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    Create9_t pCreate = d3d9 ? (Create9_t)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
    IDirect3D9 *d3d = pCreate ? pCreate(D3D_SDK_VERSION) : NULL;
    if (!d3d) { logmsg("no Direct3D - the clock stays off"); return 0; }
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mtprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("mtprobe", "mt", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hwnd;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (SUCCEEDED(hr) && dev) { hook_device(dev); IDirect3DDevice9_Release(dev); }
    else logmsg("NULLREF device failed hr=0x%08lx - the clock stays off (no retries)", (unsigned long)hr);
    IDirect3D9_Release(d3d); if (hwnd) DestroyWindow(hwnd);
    return SUCCEEDED(hr);
}

static DWORD WINAPI worker(LPVOID _) {
    gf_load(&g_gf, GAME_FONT, NULL);                                            // from the game's files; on failure Book Antiqua
    const char *vers[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "d3dx9_38.dll", "d3dx9_35.dll", "d3dx9_34.dll", "d3dx9_31.dll", "d3dx9_30.dll", NULL };
    for (int i = 0; vers[i]; i++) {
        HMODULE h = LoadLibraryA(vers[i]); if (!h) continue;
        p_D3DXCreateFontA = (D3DXCreateFontA_t)GetProcAddress(h, "D3DXCreateFontA");
        p_D3DXCreateSprite = (D3DXCreateSprite_t)GetProcAddress(h, "D3DXCreateSprite");
        if (p_D3DXCreateFontA && p_D3DXCreateSprite) { logmsg("using %s", vers[i]); break; }
    }
    if (!p_D3DXCreateFontA || !p_D3DXCreateSprite) { logmsg("no d3dx9_*.dll with D3DXCreateFontA - the clock can't be drawn"); return 0; }
    int sim = 0, d3d = 0;
    for (int i = 0; i < 120 && !(sim && d3d); i++) {                            // only cheap checks here: no devices, no windows
        if (!sim && (sim = iat_hook(IAT_SIM, "SimEngine.dll", hk_sim, (void **)&o_sim))) {
            int pre = iat_hook(IAT_SIM_PRE, "SimEngine.dll", hk_pre, (void **)&o_pre);
            int post = iat_hook(IAT_SIM_POST, "SimEngine.dll", hk_post, (void **)&o_post);
            logmsg("sim hooks installed: tick yes, match start %s, match end %s%s", pre ? "yes" : "NO", post ? "yes" : "NO",
                   post ? "" : " - the clock will stay on the post-game screen");
        }
        if (!d3d && (d3d = hook_create9())) logmsg("Direct3DCreate9 import hooked in %d place(s) after %d s", d3d, i);
        if (!(sim && d3d)) Sleep(1000);
    }
    if (!sim) { logmsg("World::Simulate import not found at %08X - not DOW2.exe 3.19? The clock stays off", IAT_SIM); return 0; }
    if (!d3d) { logmsg("d3d9.dll never came - the clock stays off"); return 0; }
    Sleep(30000);                                                               // the game creates its device at start
    if (!g_devHooked && !g_totalCalls) { logmsg("the game made its device before we came - one NULLREF try"); hook_by_nullref(); }
    return 0;
}

// one copy per game process: a patch can come both from Elite_DLLs (launcher) and from dow2-patches\ (winmm loader)
static int already_loaded(const char *name) {
    char n[96]; wsprintfA(n, "Local\\dow2patch_%s_%lu", name, GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, n);
    return m && GetLastError() == ERROR_ALREADY_EXISTS;
}
#ifdef MT_EMBED
// called by the host DLL once: dir = its folder (with a trailing backslash), ini = its ini file
void mt_start(const wchar_t *dir, const wchar_t *ini) {
    _snwprintf(g_log, MAX_PATH, L"%lsMatchTimer.log", dir); lstrcpynW(g_ini, ini, MAX_PATH);
    load_ini();
    logmsg("MatchTimer v3 inside LobbyStats, clock %s", g_enabled ? "on" : "off ([MatchTimer] Enabled=0)");
    if (already_loaded("MatchTimer")) { g_clockOff = 1; logmsg("a stand-alone MatchTimer.dll is active too: it draws the clock, not us"); }
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));   // the device hook is needed for the rest even without the clock
}
#else
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wcscpy(g_ini, g_log); wcscat(g_ini, L"MatchTimer.ini"); wcscat(g_log, L"MatchTimer.log");
    load_ini();
    logmsg("MatchTimer v3");
    if (already_loaded("MatchTimer")) { logmsg("another MatchTimer copy is already active in this game, this one stays idle"); return TRUE; }
    if (!g_enabled) { logmsg("disabled in MatchTimer.ini"); return TRUE; }
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}
#endif
