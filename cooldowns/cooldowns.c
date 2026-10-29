// cooldowns.c - seconds left on recharging abilities and global abilities, in the middle of the button over the game's
// own dark recharge sweep. Built into LobbyStats.dll (it owns the one Direct3D hook, matchtimer.c with MT_EMBED).
//
// How the game shows a recharge (found with a probe on 2026-10-29, DOW2.exe 3.19 + Elite 3.2):
//   the HUD buttons (ability_btn00..06 of the selection panel 0xB9AF80, global_abilities/global_btn00.. of 0xB5B850) are
//   button widgets (class size 0xED4); every frame the panel calls the button's SetProgress 0x688620 (usercall:
//   EAX = button, [esp+4] = float done 0..1), which turns it into frame 1..101 of "<button>/disabled/progress" (path at
//   button+0xDD0). Abilities: 0 = ready, otherwise the share of the recharge done (each frame 0 first, then the share);
//   global abilities: 1 - ticks left / recharge ticks, 1 = ready.
// We only read: a detour on SetProgress keeps the latest share of each button and the game time it came at (game ms of
// matchtimer.c, pauses not counted); the share grows evenly with game time, so two samples give the rate and the rate
// the seconds left. The button's place on the screen comes from the game's GetBounds 0x6859E0 (widget, clip path,
// float out[4] = x0 x1 y0 y1, 2 = window pixels), asked on the UI thread inside the detour, once a second per button.
// Nothing in the simulation is touched: no desync, the other players don't need it.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define SETPROGRESS_ADDR 0x00688620
static const unsigned char SETPROGRESS_PRO[9] = {0x51, 0xF3, 0x0F, 0x10, 0x05, 0x98, 0xDF, 0x07, 0x01};   // push ecx; movss xmm0,[0x0107DF98]
#define GETBOUNDS_ADDR   0x006859E0
static const unsigned char GETBOUNDS_PRO[2] = {0x6A, 0xFF};                                               // push -1
typedef int (__cdecl *GetBounds_t)(void *widget, const char *path, float *out, int mode);

DWORD mt_game_ms(void); int mt_ticking(void); float mt_ms_per_tick(void); long mt_ticks(void);

static wchar_t g_log[MAX_PATH];
static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

// ---------- settings: LobbyStats.ini, [Cooldowns] ----------
static int g_on = 1, g_size = 29;                   // Size: height of the digits' line in px at 1440 (scaled by the screen height)
static DWORD g_color = 0xFFFFF8F0;                   // the clock's colour

// ---------- per button: the latest share and the rate ----------
#define SLOTS 24                                     // 0..15 ability_btnNN, 16..23 global_btnNN
typedef struct {
    void *w;                                         // the button widget
    float p, p0; DWORD t, t0;                        // latest share at sim tick t; first share of this recharge at tick t0
    DWORD seen;                                      // GetTickCount() of the latest share in (0,1)
    int shown;                                       // seconds on the screen: within one recharge they only go down
    volatile int ready;                              // the latest value said ready (0 or 1), or the button is hidden
    int exact; DWORD exactLeft, exactAt;             // global abilities: game ms left read from the engine at game ms exactAt
    DWORD boundsAt; float x0, x1, y0, y1; int haveBounds;
} Slot;
static Slot g_slot[SLOTS];
static CRITICAL_SECTION g_cs;
static unsigned g_named;                             // slots already written to the log (bit per slot)
#define GONE_MS 100                                  // a button the game stopped updating (deselected, recharged) for this long is gone

static int slot_of(const char *path) {
    const char *a = strstr(path, "ability_btn"), *g = strstr(path, "global_btn");
    if (a && a[11] >= '0' && a[11] <= '9' && a[12] >= '0' && a[12] <= '9') { int n = (a[11] - '0') * 10 + a[12] - '0'; return n < 16 ? n : -1; }
    if (g && g[10] >= '0' && g[10] <= '9' && g[11] >= '0' && g[11] <= '9') { int n = (g[10] - '0') * 10 + g[11] - '0'; return n < 8 ? 16 + n : -1; }
    return -1;
}

// the start of each recharge per ability of each owner (squad): switching the selection back and forth keeps the rate,
// so the number comes at once instead of after two sim ticks
typedef struct { unsigned key; float p0, p; DWORD t0; } Seen;
static Seen g_seen[64]; static int g_nseen;
static void cd_share(char *w, float p, unsigned key);
void cd_progress(char *w, float p) __asm__("cd_progress");
void cd_progress(char *w, float p) { cd_share(w, p, 0); }
static void cd_share(char *w, float p, unsigned key) {
    if (!g_on || !w || !mt_ticking()) return;
    const char *path = w + 0xDD0;
    if (IsBadReadPtr(path, 16) || path[0] != '/') return;
    int s = slot_of(path); if (s < 0) return;
    // ready (0 or 1): the number goes at once - after a selection change the button may hold another, ready ability.
    // The game sends 0 and then the share every frame of a recharge, so this only marks; the share clears it again.
    if (!(p > 0.0f) || p >= 0.999f) { g_slot[s].ready = 1; return; }
    g_slot[s].ready = 0;
    if (!(g_named & 1u << s)) { g_named |= 1u << s; logmsg("button %.80s -> slot %d (share %.3f)", path, s, p); }
    // sim ticks, not game ms: the share grows by the same step every tick, so one tick gives the exact rate (game ms are
    // the wall-clock gaps between ticks and made the first seconds jump by one)
    DWORD now = GetTickCount(), gt = (DWORD)mt_ticks();
    EnterCriticalSection(&g_cs);
    Slot *b = &g_slot[s];
    // a new recharge, another ability on this button (the selection changed) or a gap: start the rate again
    float expect = b->p + 0.05f;
    if (b->w != w || now - b->seen > 500 || p < b->p - 0.002f || (b->t != gt && p > expect && gt - b->t < 10)) {
        b->w = w; b->p0 = p; b->t0 = gt; b->shown = 0;
    }
    if (key) {                                       // this ability of this owner seen before in this recharge: its start
        Seen *k = NULL;
        for (int i = 0; i < g_nseen; i++) if (g_seen[i].key == key) { k = &g_seen[i]; break; }
        if (!k) { k = &g_seen[g_nseen < 64 ? g_nseen++ : (int)(key % 64)]; k->key = key; k->p0 = p; k->t0 = gt; }
        else if (p < k->p - 0.002f) { k->p0 = p; k->t0 = gt; }                 // a new recharge
        k->p = p;
        if (k->t0 < b->t0 && k->p0 < p) { b->p0 = k->p0; b->t0 = k->t0; }       // an older start: the rate at once
    }
    b->p = p; b->t = gt; b->seen = now;
    LeaveCriticalSection(&g_cs);
    if (!b->haveBounds || now - b->boundsAt > 1000) {                           // place of the button, UI thread
        float o[4] = {0, 0, 0, 0};
        int ok = ((GetBounds_t)GETBOUNDS_ADDR)(w, path, o, 2);
        EnterCriticalSection(&g_cs);
        b->boundsAt = now;
        if (ok && o[1] > o[0] + 4 && o[3] > o[2] + 4) { b->x0 = o[0]; b->x1 = o[1]; b->y0 = o[2]; b->y1 = o[3]; b->haveBounds = 1; }
        LeaveCriticalSection(&g_cs);
    }
}

void *g_spTramp __asm__("cd_sp_tramp");
__attribute__((naked)) static void hk_setprogress(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushl 36(%esp)\n\t"                         // the share (float, [esp+4] at the call)
        "pushl %eax\n\t"                             // the button
        "call cd_progress\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "jmp *cd_sp_tramp\n\t");
}

// The selection panel's ability button update (virtual, slot 0x0111AFBC = 0xB9AF80; this = the button's slot struct,
// +0x28 the button widget, +0x74 the ability, +0x2A8 the owner the panel asked about) asks the engine why the ability
// can't be used (iface->vt+0xB0) and only for a recharge (reasons 0xD/0xE) asks iface->vt+0xB4 for the share done.
// Out of energy the reasons have no recharge, so the sweep and our share were missing: after the update we ask
// vt+0xB4 ourselves - the same read-only question the panel asks.
#define BTNUPD_SLOT 0x0111AFBC
#define BTNUPD_FN   0x00B9AF80
typedef void (__fastcall *BtnUpd_t)(char *btn, void *edx, char *ctx);
typedef float (__thiscall *Share_t)(void *iface, void *owner, int ability);
static BtnUpd_t o_btnupd;
static void __fastcall hk_btnupd(char *btn, void *edx, char *ctx) {
    o_btnupd(btn, edx, ctx);
    if (!g_on || !btn || !ctx || !mt_ticking()) return;
    char *w = *(char **)(btn + 0x28); int ability = *(int *)(btn + 0x74);
    void **iface = *(void ***)(ctx + 0xC);
    if (!w) return;
    if (!ability || !iface || *(w + 0x209) != 1) { cd_progress(w, 0.0f); return; }   // no ability or the button is hidden: no number
    Share_t share = (Share_t)(*(void ***)iface)[0xB4 / 4];
    unsigned key = (unsigned)ability * 2654435761u;  // the ability and its owner (ConstOwnerPointer, 16 bytes)
    for (int i = 0; i < 4; i++) key = (key ^ ((unsigned *)(btn + 0x2A8))[i]) * 16777619u;
    cd_share(w, share(iface, btn + 0x2A8, ability), key | 1);
}
// Global abilities: their panel's button update (virtual, slot 0x01118B4C = 0xB5B850; this = the button's slot struct,
// +0x28 the widget, arg +0x10 the ability's entity) takes the entity's extension 1 (EntityController::QI through the import
// 0x00F88338) whose +0x14 is the sim ticks left; it shows 1 - left / total. We read the ticks left after it: exact
// seconds from the first frame, no rate to guess (the guess made the number come late).
#define GLOBUPD_SLOT 0x01118B4C
#define GLOBUPD_FN   0x00B5B850
#define IAT_QI       0x00F88338
typedef void *(__thiscall *QI_t)(void *controller, unsigned id);
static BtnUpd_t o_globupd;
static void __fastcall hk_globupd(char *btn, void *edx, char *ctx) {
    o_globupd(btn, edx, ctx);
    if (!g_on || !btn || !ctx || !mt_ticking()) return;
    char *w = *(char **)(btn + 0x28), *ent = *(char **)(ctx + 0x10);
    if (!w || !ent) return;
    void *ec = *(void **)(ent + 0x3C); QI_t qi = *(QI_t *)IAT_QI;
    char *ext = ec && qi ? qi(ec, 1) : NULL;
    if (!ext) return;
    const char *path = w + 0xDD0;
    if (IsBadReadPtr(path, 16) || path[0] != '/') return;
    int s = slot_of(path), left = *(int *)(ext + 0x14); if (s < 0) return;
    EnterCriticalSection(&g_cs);
    Slot *b = &g_slot[s];
    if (left > 0) { b->exact = 1; b->exactLeft = (DWORD)(left * mt_ms_per_tick()); b->exactAt = mt_game_ms(); }
    else b->exact = 0;
    LeaveCriticalSection(&g_cs);
}
static void install_btnupd(void) {
    DWORD *gs = (DWORD *)GLOBUPD_SLOT, go;
    if (!IsBadReadPtr(gs, 4) && *gs == GLOBUPD_FN && !IsBadReadPtr((void *)IAT_QI, 4) && VirtualProtect(gs, 4, PAGE_READWRITE, &go)) {
        o_globupd = (BtnUpd_t)GLOBUPD_FN; *gs = (DWORD)hk_globupd; VirtualProtect(gs, 4, go, &go);
        logmsg("hooked global ability button update (vtable %08X): exact seconds", GLOBUPD_SLOT);
    } else logmsg("global ability button update not at %08X: guessed seconds", GLOBUPD_SLOT);
    DWORD *slot = (DWORD *)BTNUPD_SLOT; DWORD old;
    if (IsBadReadPtr(slot, 4) || *slot != BTNUPD_FN) { logmsg("ability button update not at %08X: numbers only with the game's own sweep", BTNUPD_SLOT); return; }
    o_btnupd = (BtnUpd_t)BTNUPD_FN;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
    *slot = (DWORD)hk_btnupd;
    VirtualProtect(slot, 4, old, &old);
    logmsg("hooked ability button update (vtable %08X): numbers also out of energy", BTNUPD_SLOT);
}

// ---------- drawing (EndScene of matchtimer.c, through LobbyStats' ls_draw) ----------
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
static D3DXCreateFontA_t p_CreateFont; static D3DXCreateSprite_t p_CreateSprite;
#define D3DXSPRITE_ALPHABLEND 0x10
#include "../common/gamefont.h"
static GameFont g_gf = { "0123456789:" };            // the digits in the game's Caslon Antique New
static volatile int g_ready;
static ID3DXSprite *g_sprite; static ID3DXFont *g_font; static IDirect3DDevice9 *g_dev; static int g_fontPx, g_lost;

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
static int ensure(IDirect3DDevice9 *dev, int px) {
    if (g_dev != dev) {
        if (g_sprite) { g_sprite->lpVtbl->Release(g_sprite); g_sprite = NULL; }
        if (g_font) { g_font->lpVtbl->Release(g_font); g_font = NULL; }
        g_dev = dev; g_fontPx = 0;
        if (FAILED(p_CreateSprite(dev, &g_sprite))) { g_sprite = NULL; logmsg("sprite failed"); }
    }
    if (!g_sprite) return 0;
    if (gf_ready(&g_gf, dev, px)) return 1;
    if (g_fontPx != px) {                            // no game font: Windows Book Antiqua, bold
        if (g_font) { g_font->lpVtbl->Release(g_font); g_font = NULL; }
        g_fontPx = px;
        if (FAILED(p_CreateFont(dev, px, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Book Antiqua", &g_font))) g_font = NULL;
    }
    return g_font != NULL;
}
static void text(const char *t, RECT *r, DWORD c, int game) {
    if (game) gf_draw(&g_gf, g_sprite, t, r, DT_CENTER | DT_VCENTER, c);
    else g_font->lpVtbl->DrawTextA(g_font, g_sprite, t, -1, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP, c);
}
void cd_draw(IDirect3DDevice9 *dev) {
    if (!g_on || !g_ready || g_lost || !mt_ticking()) return;
    int W, H; if (!on_back_buffer(dev, &W, &H)) return;
    D3DDEVICE_CREATION_PARAMETERS cp; RECT cr;
    float sx = 1, sy = 1;                            // window pixels (GetBounds) -> back buffer pixels
    if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)) && cp.hFocusWindow && GetClientRect(cp.hFocusWindow, &cr) && cr.right > 0 && cr.bottom > 0) {
        sx = (float)W / cr.right; sy = (float)H / cr.bottom;
    }
    struct { RECT r; char t[8]; } item[SLOTS]; int n = 0;
    DWORD gt = mt_game_ms();
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    for (int s = 0; s < SLOTS; s++) {
        Slot *b = &g_slot[s];
        if (!b->w || !b->haveBounds || b->ready || now - b->seen > GONE_MS) continue;
        // the game hides a button with no ability for the new selection and stops updating it: gone in this frame
        if (IsBadReadPtr((char *)b->w + 0x209, 1) || *((char *)b->w + 0x209) != 1) continue;      // not recharging or not on screen now
        if (!b->exact && (b->t - b->t0 < 1 || b->p <= b->p0)) continue;          // the rate isn't known yet (one sim tick)
        float left;
        if (b->exact) left = (float)b->exactLeft - (float)(gt - b->exactAt);   // global ability: read, not guessed
        else {
            float rate = (b->p - b->p0) / (float)(b->t - b->t0);                 // share per sim tick
            left = (1.0f - b->p) / rate * mt_ms_per_tick();
        }
        if (left < 0) left = 0;
        int sec = (int)((left + 999.0f) / 1000.0f);
        if (sec <= 0 || sec > 3600) continue;
        if (!b->shown || sec < b->shown || sec > b->shown + 2) b->shown = sec;    // no jumping back and forth by a second
        sec = b->shown;
        if (sec < 100) sprintf(item[n].t, "%d", sec); else sprintf(item[n].t, "%d:%02d", sec / 60, sec % 60);
        item[n].r.left = (LONG)(b->x0 * sx); item[n].r.right = (LONG)(b->x1 * sx);
        item[n].r.top = (LONG)(b->y0 * sy); item[n].r.bottom = (LONG)(b->y1 * sy);
        n++;
    }
    LeaveCriticalSection(&g_cs);
    if (!n) return;
    int px = H * g_size / 1440;                     // one fixed size (a new size rasterizes the digits again) if (px < 10) px = 10; if (px > 64) px = 64;
    if (!ensure(dev, px)) return;
    int game = g_gf.ok && g_gf.tex && g_gf.px == px;
    D3DVIEWPORT9 vp, full = { 0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f };
    int haveVp = SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &vp));
    IDirect3DDevice9_SetViewport(dev, &full);
    if (SUCCEEDED(g_sprite->lpVtbl->Begin(g_sprite, D3DXSPRITE_ALPHABLEND))) {
        // no outline: the game's own text shadow, as the match clock without its box (matchtimer.c): 2 px down and a bit
        // right, a light blur around it
        int sx = H >= 1620 ? 2 : 1, sy = 2 * H / 1080 > 2 ? 2 * H / 1080 : 2;
        static const int bx[4] = {-1, 1, 0, 0}, by[4] = {0, 0, -1, 1};
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < 5; k++) {
                RECT q = item[i].r; OffsetRect(&q, sx, sy);
                DWORD c = 0xC0000000;
                if (k < 4) { OffsetRect(&q, bx[k], by[k]); c = 0x30000000; }
                text(item[i].t, &q, c, game);
            }
            text(item[i].t, &item[i].r, g_color, game);
        }
        g_sprite->lpVtbl->End(g_sprite);
    }
    if (haveVp) IDirect3DDevice9_SetViewport(dev, &vp);
}
// ID3DXSprite/ID3DXFont must let go of their video memory before a device Reset
void cd_lost(IDirect3DDevice9 *dev) {
    if (dev != g_dev || g_lost) return;
    if (g_sprite) g_sprite->lpVtbl->OnLostDevice(g_sprite);
    if (g_font) g_font->lpVtbl->OnLostDevice(g_font);
    g_lost = 1;
}
void cd_reset(IDirect3DDevice9 *dev, HRESULT hr) {
    (void)dev;
    if (!g_lost || FAILED(hr)) return;
    if (g_sprite) g_sprite->lpVtbl->OnResetDevice(g_sprite);
    if (g_font) g_font->lpVtbl->OnResetDevice(g_font);
    g_lost = 0;
}

static DWORD WINAPI cd_worker(LPVOID _) {
    gf_load(&g_gf, "Caslon Antique New", NULL);
    const char *vers[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "d3dx9_38.dll", "d3dx9_35.dll", "d3dx9_34.dll", "d3dx9_31.dll", "d3dx9_30.dll", NULL };
    for (int i = 0; vers[i]; i++) {
        HMODULE h = LoadLibraryA(vers[i]); if (!h) continue;
        p_CreateFont = (D3DXCreateFontA_t)GetProcAddress(h, "D3DXCreateFontA");
        p_CreateSprite = (D3DXCreateSprite_t)GetProcAddress(h, "D3DXCreateSprite");
        if (p_CreateFont && p_CreateSprite) break;
    }
    if (!p_CreateFont || !p_CreateSprite) { logmsg("no d3dx9_*.dll: no cooldown numbers"); return 0; }
    g_ready = 1;
    return 0;
}
static void install(void) {
    unsigned char *t = (unsigned char *)SETPROGRESS_ADDR, *gb = (unsigned char *)GETBOUNDS_ADDR;
    if (IsBadReadPtr(t, 9) || memcmp(t, SETPROGRESS_PRO, 9) || IsBadReadPtr(gb, 2) || memcmp(gb, GETBOUNDS_PRO, 2)) {
        logmsg("DOW2.exe differs at %08X/%08X: no cooldown numbers", SETPROGRESS_ADDR, GETBOUNDS_ADDR); g_on = 0; return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, SETPROGRESS_PRO, 9); tr[9] = 0xE9; *(DWORD *)(tr + 10) = (SETPROGRESS_ADDR + 9) - (DWORD)(tr + 14);
    g_spTramp = tr;
    DWORD old; VirtualProtect(t, 9, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)hk_setprogress - (DWORD)(t + 5); t[5] = t[6] = t[7] = t[8] = 0x90;
    VirtualProtect(t, 9, old, &old); FlushInstructionCache(GetCurrentProcess(), t, 9);
    logmsg("hooked button SetProgress at %08X", SETPROGRESS_ADDR);
}

// called by LobbyStats once: dir = its folder (with a trailing backslash), ini = its ini file
void cd_start(const wchar_t *dir, const wchar_t *ini) {
    _snwprintf(g_log, MAX_PATH, L"%lsCooldowns.log", dir);
    g_on = GetPrivateProfileIntW(L"Cooldowns", L"Enabled", 1, ini);
    g_size = GetPrivateProfileIntW(L"Cooldowns", L"Size", g_size, ini);
    if (g_size < 12) g_size = 12; if (g_size > 64) g_size = 64;
    wchar_t c[16]; GetPrivateProfileStringW(L"Cooldowns", L"Color", L"FFF8F0", c, 16, ini);
    g_color = 0xFF000000 | (wcstoul(c, NULL, 16) & 0xFFFFFF);
    logmsg("Cooldowns 1 (%s)", g_on ? "on" : "off, [Cooldowns] Enabled=0");
    if (!g_on) return;
    InitializeCriticalSection(&g_cs);
    install(); if (g_on) install_btnupd();
    if (g_on) CloseHandle(CreateThread(NULL, 0, cd_worker, NULL, 0, NULL));
}
