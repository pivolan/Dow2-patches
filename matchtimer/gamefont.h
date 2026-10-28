// gamefont.h - the clock's digits in the game's own font, read from the player's game files at run time.
//
// The game draws its titles and numbers with "Caslon Antique New", a Flash font (DefineFont3) inside
// ui\movies\fonts\master_fonts.gfx in GameAssets\Archives\000_data.sga. It isn't a Windows font and we don't ship it:
// at start the DLL finds the file in the archive (SGA v5), inflates it, takes the glyph outlines of "0123456789:"
// and later rasterizes them into a texture drawn with the sprite. Anything unexpected: gf_load() fails, the log
// says why and the clock keeps a Windows font.
//
// SGA v5: header +172 TOC size, +176 data start, +180 TOC offset. TOC: drives (+0), folders (+6), files (+12),
// strings (+18), each {u32 offset from the TOC, u16 count}. File entry, 22 bytes: name offset (in the strings),
// data offset (from the data start), compressed size, size, time, flags. Data: a zlib stream.

#define GF_CHARS "0123456789:"
#define GF_N     11
#define GF_MAXPT 4096

typedef struct { float x0, y0, x1, y1; } GfSeg;
static struct {
    int ok; char name[64];
    float ascent, descent;                  // font units (1024 * 20 per em), y down
    float adv[GF_N];
    GfSeg *seg[GF_N]; int nseg[GF_N];
} g_gf;

// ---------- inflate (RFC 1951; after puff.c by Mark Adler) ----------
typedef struct { const BYTE *in; size_t inlen, inpos; DWORD bitbuf; int bitcnt; BYTE *out; size_t outlen, outpos; } Inf;
typedef struct { short count[16], symbol[288]; } Huff;
static int inf_bits(Inf *s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) return -1;
        val |= (long)s->in[s->inpos++] << s->bitcnt; s->bitcnt += 8;
    }
    s->bitbuf = (DWORD)(val >> need); s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}
static int inf_decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1); if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int inf_build(Huff *h, const short *len, int n) {
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return 0;
}
static int inf_codes(Inf *s, const Huff *lc, const Huff *dc) {
    static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,
                                    6145,8193,12289,16385,24577};
    static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = inf_decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (s->outpos >= s->outlen) return -1; s->out[s->outpos++] = (BYTE)sym; continue; }
        if (sym == 256) return 0;
        sym -= 257; if (sym >= 29) return -1;
        int e = inf_bits(s, lext[sym]); if (e < 0) return -1;
        int len = lbase[sym] + e;
        int ds = inf_decode(s, dc); if (ds < 0 || ds >= 30) return -1;
        e = inf_bits(s, dext[ds]); if (e < 0) return -1;
        size_t dist = dbase[ds] + e;
        if (dist > s->outpos || s->outpos + len > s->outlen) return -1;
        while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
    }
}
static int inflate_raw(const BYTE *in, size_t inlen, BYTE *out, size_t outlen) {
    static Huff lc, dc;
    Inf s = { in, inlen, 0, 0, 0, out, outlen, 0 };
    int last;
    do {
        last = inf_bits(&s, 1); int type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {                                             // stored
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) return -1;
            unsigned len = s.in[s.inpos] | s.in[s.inpos + 1] << 8; s.inpos += 4;
            if (s.inpos + len > s.inlen || s.outpos + len > s.outlen) return -1;
            memcpy(s.out + s.outpos, s.in + s.inpos, len); s.inpos += len; s.outpos += len;
        } else if (type == 1) {                                      // fixed codes
            short l[288]; int i;
            for (i = 0; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            inf_build(&lc, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            inf_build(&dc, l, 30);
            if (inf_codes(&s, &lc, &dc)) return -1;
        } else if (type == 2) {                                      // dynamic codes
            static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            short l[320]; int nlen = inf_bits(&s, 5) + 257, ndist = inf_bits(&s, 5) + 1, ncode = inf_bits(&s, 4) + 4;
            if (nlen > 286 || ndist > 30 || ncode < 4) return -1;
            int i;
            for (i = 0; i < 19; i++) l[order[i]] = 0;
            for (i = 0; i < ncode; i++) { int b = inf_bits(&s, 3); if (b < 0) return -1; l[order[i]] = (short)b; }
            inf_build(&lc, l, 19);
            for (i = 0; i < nlen + ndist; ) {
                int sym = inf_decode(&s, &lc), rep, val = 0;
                if (sym < 0) return -1;
                if (sym < 16) { l[i++] = (short)sym; continue; }
                if (sym == 16) { if (!i) return -1; val = l[i - 1]; rep = 3 + inf_bits(&s, 2); }
                else if (sym == 17) rep = 3 + inf_bits(&s, 3);
                else rep = 11 + inf_bits(&s, 7);
                if (i + rep > nlen + ndist) return -1;
                while (rep--) l[i++] = (short)val;
            }
            inf_build(&lc, l, nlen);
            inf_build(&dc, l + nlen, ndist);
            if (inf_codes(&s, &lc, &dc)) return -1;
        } else return -1;
    } while (!last);
    return (int)s.outpos;
}

// ---------- SGA: one file out of an archive ----------
static BYTE *sga_read(const wchar_t *path, const char *want, DWORD *size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { char a[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, path, -1, a, sizeof a, NULL, NULL); logmsg("game font: can't open %s (%lu)", a, GetLastError()); return NULL; }
    BYTE h[184], *toc = NULL, *packed = NULL, *out = NULL; DWORD got;
    LARGE_INTEGER at;
    if (!ReadFile(f, h, sizeof h, &got, NULL) || got != sizeof h || memcmp(h, "_ARCHIVE", 8) || *(DWORD *)(h + 8) != 5) { logmsg("game font: not an SGA v5 archive"); goto done; }
    DWORD tocSize = *(DWORD *)(h + 172), dataStart = *(DWORD *)(h + 176), tocOff = *(DWORD *)(h + 180);
    if (tocSize < 24 || tocSize > 64u << 20) { logmsg("game font: odd TOC size %lu", tocSize); goto done; }
    toc = malloc(tocSize); at.QuadPart = tocOff;
    if (!toc || !SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !ReadFile(f, toc, tocSize, &got, NULL) || got != tocSize) { logmsg("game font: TOC read failed"); goto done; }
    DWORD fo = *(DWORD *)(toc + 12), so = *(DWORD *)(toc + 18); WORD fc = *(WORD *)(toc + 16);
    if (fo + fc * 22u > tocSize || so >= tocSize) { logmsg("game font: odd TOC (files %lu+%u, strings %lu)", fo, fc, so); goto done; }
    for (unsigned i = 0; i < fc; i++) {
        BYTE *e = toc + fo + i * 22;
        DWORD no = so + *(DWORD *)e;
        if (no >= tocSize || strncmp((char *)toc + no, want, tocSize - no)) continue;
        DWORD off = *(DWORD *)(e + 4), csz = *(DWORD *)(e + 8), dsz = *(DWORD *)(e + 12);
        if (csz < 2 || csz > 64u << 20 || dsz > 64u << 20) { logmsg("game font: %s: odd sizes %lu/%lu", want, csz, dsz); break; }
        packed = malloc(csz); out = malloc(dsz ? dsz : 1); at.QuadPart = (LONGLONG)dataStart + off;
        if (!packed || !out || !SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !ReadFile(f, packed, csz, &got, NULL) || got != csz) { logmsg("game font: %s: read failed", want); break; }
        if (csz == dsz) { memcpy(out, packed, dsz); *size = dsz; }
        else if (inflate_raw(packed + 2, csz - 2, out, dsz) == (int)dsz) *size = dsz;   // skip the zlib header
        else { logmsg("game font: %s: inflate failed (%d of %lu bytes)", want, inflate_raw(packed + 2, csz - 2, out, dsz), dsz); break; }
        free(packed); free(toc); CloseHandle(f);
        return out;
    }
done:
    free(packed); free(out); free(toc); CloseHandle(f);
    return NULL;
}

// ---------- SWF/GFX: the outlines of one DefineFont3 ----------
typedef struct { const BYTE *p; size_t len, pos; int bit; } Bits;
static unsigned ub(Bits *b, int n) {
    unsigned v = 0;
    while (n--) {
        if (b->pos >= b->len) return v;
        v = v << 1 | (b->p[b->pos] >> (7 - b->bit) & 1);
        if (++b->bit == 8) { b->bit = 0; b->pos++; }
    }
    return v;
}
static int sb(Bits *b, int n) { unsigned v = ub(b, n); return n && v >> (n - 1) & 1 ? (int)(v | ~0u << n) : (int)v; }
// one glyph's SHAPE: lines and quadratic curves (flattened) as segments, in font units
static int gf_shape(Bits *b, GfSeg *out, int cap) {
    int nf = ub(b, 4), nl = ub(b, 4), n = 0; float x = 0, y = 0;
    for (;;) {
        if (ub(b, 1) == 0) {                                         // style change / end
            int fl = ub(b, 5);
            if (!fl) break;
            if (fl & 1) { int nb = ub(b, 5); x = (float)sb(b, nb); y = (float)sb(b, nb); }
            if (fl & 2) ub(b, nf);
            if (fl & 4) ub(b, nf);
            if (fl & 8) ub(b, nl);
            if (fl & 16) return -1;                                  // new styles: not in font glyphs
        } else if (ub(b, 1)) {                                       // straight edge
            int nb = ub(b, 4) + 2; float dx = 0, dy = 0;
            if (ub(b, 1)) { dx = (float)sb(b, nb); dy = (float)sb(b, nb); }
            else if (ub(b, 1)) dy = (float)sb(b, nb); else dx = (float)sb(b, nb);
            if (n < cap) out[n++] = (GfSeg){ x, y, x + dx, y + dy };
            x += dx; y += dy;
        } else {                                                     // curved edge: flatten in 8 steps
            int nb = ub(b, 4) + 2;
            float cx = x + sb(b, nb), cy = y + sb(b, nb), ax, ay;
            ax = cx + sb(b, nb); ay = cy + sb(b, nb);
            float px = x, py = y;
            for (int k = 1; k <= 8; k++) {
                float t = k / 8.0f, u = 1 - t;
                float qx = u * u * x + 2 * u * t * cx + t * t * ax, qy = u * u * y + 2 * u * t * cy + t * t * ay;
                if (n < cap) out[n++] = (GfSeg){ px, py, qx, qy };
                px = qx; py = qy;
            }
            x = ax; y = ay;
        }
        if (b->pos >= b->len) return -1;
    }
    return n;
}
static int gf_font3(const BYTE *t, size_t len, const char *want) {
    if (len < 6) return 0;
    BYTE flags = t[2]; int nameLen = t[4];
    if (5 + nameLen + 2 > (int)len) return 0;
    char name[256]; memcpy(name, t + 5, nameLen); name[nameLen] = 0;
    if (nameLen && !name[nameLen - 1]) nameLen--;
    if (strcmp(name, want)) return 0;
    size_t p = 5 + t[4]; int ng = t[p] | t[p + 1] << 8; p += 2;
    int wideOff = flags & 8, os = wideOff ? 4 : 2;
    size_t tab = p;
    if (tab + (ng + 1) * os > len) return -1;
    #define OFS(i) (wideOff ? *(DWORD *)(t + tab + (i) * 4) : *(WORD *)(t + tab + (i) * 2))
    size_t codeTab = tab + OFS(ng);
    if (codeTab + ng * 2 > len) return -1;
    float adv[1024] = {0}; int hasLayout = flags & 0x80;
    size_t lay = codeTab + ng * 2;
    if (hasLayout && lay + 6 + ng * 2 <= len) {
        g_gf.ascent = *(short *)(t + lay); g_gf.descent = *(short *)(t + lay + 2);
        for (int i = 0; i < ng && i < 1024; i++) adv[i] = *(short *)(t + lay + 6 + i * 2);
    }
    static GfSeg tmp[GF_MAXPT];
    int found = 0;
    for (int c = 0; c < GF_N; c++) {
        int code = GF_CHARS[c], gi = -1;
        for (int i = 0; i < ng; i++) if (*(WORD *)(t + codeTab + i * 2) == code) { gi = i; break; }
        if (gi < 0) continue;
        Bits b = { t + tab + OFS(gi), (gi + 1 < ng ? OFS(gi + 1) : OFS(ng)) - OFS(gi), 0, 0 };
        int n = gf_shape(&b, tmp, GF_MAXPT);
        if (n <= 0) continue;
        g_gf.seg[c] = malloc(n * sizeof(GfSeg)); if (!g_gf.seg[c]) continue;
        memcpy(g_gf.seg[c], tmp, n * sizeof(GfSeg)); g_gf.nseg[c] = n;
        g_gf.adv[c] = gi < 1024 ? adv[gi] : 0;
        if (g_gf.adv[c] <= 0) {                                      // no layout: the outline's width plus a little
            float mx = 0; for (int i = 0; i < n; i++) if (tmp[i].x1 > mx) mx = tmp[i].x1;
            g_gf.adv[c] = mx + 1500;
        }
        found++;
    }
    #undef OFS
    if (!hasLayout || g_gf.ascent <= 0) { g_gf.ascent = 20480 * 0.9f; g_gf.descent = 20480 * 0.25f; }
    lstrcpynA(g_gf.name, want, sizeof g_gf.name);
    return found;
}
// read the font named `want` from the game's master_fonts (gameDir NULL = the folder of the running exe); 1 = ok
static int gf_load(const char *want, const wchar_t *gameDir) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (gameDir) _snwprintf(dir, MAX_PATH, L"%ls\\x", gameDir); else GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0; else dir[0] = 0;
    BYTE *swf = NULL; DWORD size = 0;
    for (int i = 0; i < 10 && !swf; i++) {
        _snwprintf(path, MAX_PATH, L"%lsGameAssets\\Archives\\%03d_data.sga", dir, i);
        swf = sga_read(path, "master_fonts.gfx", &size);
    }
    if (!swf) { logmsg("game font: master_fonts.gfx not found in GameAssets\\Archives\\*_data.sga"); return 0; }
    int found = 0;
    if (size > 21 && (!memcmp(swf, "GFX", 3) || !memcmp(swf, "FWS", 3))) {
        Bits b = { swf + 8, size - 8, 0, 0 };
        int nb = ub(&b, 5); size_t p = 8 + (5 + 4 * nb + 7) / 8 + 4;   // skip the frame RECT, rate and count
        while (p + 2 <= size && !found) {
            WORD ch = *(WORD *)(swf + p); p += 2;
            DWORD code = ch >> 6, len = ch & 63;
            if (len == 63) { if (p + 4 > size) break; len = *(DWORD *)(swf + p); p += 4; }
            if (p + len > size || code == 0) break;
            if (code == 75) found = gf_font3(swf + p, len, want);    // DefineFont3
            p += len;
        }
    }
    free(swf);
    if (found < GF_N) {
        logmsg("game font: \"%s\" %s in master_fonts.gfx", want, found > 0 ? "lacks some digits" : "not found");
        for (int c = 0; c < GF_N; c++) { free(g_gf.seg[c]); g_gf.seg[c] = NULL; }
        return 0;
    }
    g_gf.ok = 1;
    logmsg("game font: \"%s\" from master_fonts.gfx (ascent %.0f, descent %.0f)", want, g_gf.ascent, g_gf.descent);
    return 1;
}

// ---------- the digits as a texture: white, alpha = coverage (4x4 samples, even-odd) ----------
#define GF_M 3                                                       // margin around each glyph for the outline
static IDirect3DTexture9 *g_gfTex; static IDirect3DDevice9 *g_gfDev; static int g_gfPx;
static RECT g_gfSrc[GF_N]; static int g_gfAdv[GF_N], g_gfCell;     // source rects, advances and line height in px
static void gf_raster(const GfSeg *sg, int n, float s, float ox, float oy, int w, int h, BYTE *out, int pitch) {
    float acc[512], xs[256];
    if (w > 512) return;
    for (int y = 0; y < h; y++) {
        memset(acc, 0, sizeof(float) * w);
        for (int sy = 0; sy < 4; sy++) {
            float Y = y + (sy + 0.5f) / 4; int nx = 0;
            for (int i = 0; i < n && nx < 256; i++) {
                float y0 = oy + sg[i].y0 * s, y1 = oy + sg[i].y1 * s;
                if ((y0 <= Y && Y < y1) || (y1 <= Y && Y < y0)) {
                    float x0 = ox + sg[i].x0 * s, x1 = ox + sg[i].x1 * s;
                    xs[nx++] = x0 + (Y - y0) * (x1 - x0) / (y1 - y0);
                }
            }
            for (int i = 1; i < nx; i++) { float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
            for (int i = 0; i + 1 < nx; i += 2) {                    // even-odd spans, exact horizontal coverage
                float a = xs[i] < 0 ? 0 : xs[i], b = xs[i + 1] > w ? w : xs[i + 1];
                for (int px = (int)a; px < w && px < b; px++) {
                    float l = a > px ? a : px, r = b < px + 1 ? b : px + 1;
                    if (r > l) acc[px] += (r - l) / 4;
                }
            }
        }
        DWORD *row = (DWORD *)(out + y * pitch);
        for (int x = 0; x < w; x++) { float c = acc[x] > 1 ? 1 : acc[x]; row[x] = (DWORD)(c * 255 + 0.5f) << 24 | 0xFFFFFF; }
    }
}
static int gf_ready(IDirect3DDevice9 *dev, int px) {
    if (!g_gf.ok) return 0;
    if (g_gfTex && g_gfDev == dev && g_gfPx == px) return 1;
    if (g_gfTex) { IDirect3DTexture9_Release(g_gfTex); g_gfTex = NULL; }
    g_gfDev = dev; g_gfPx = px;
    float s = px / (g_gf.ascent + g_gf.descent);                     // px = the line's height, like a Windows font's
    int cell = (int)(px + 0.999f) + 2 * GF_M, tw = 0;
    for (int c = 0; c < GF_N; c++) { g_gfAdv[c] = (int)(g_gf.adv[c] * s + 0.5f); tw += g_gfAdv[c] + 2 * GF_M + 2; }
    if (tw > 4096 || FAILED(IDirect3DDevice9_CreateTexture(dev, tw, cell, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_gfTex, NULL))) {
        g_gfTex = NULL; g_gf.ok = 0; logmsg("game font: texture %dx%d failed, back to a Windows font", tw, cell); return 0;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_gfTex, 0, &lr, NULL, 0))) { IDirect3DTexture9_Release(g_gfTex); g_gfTex = NULL; g_gf.ok = 0; return 0; }
    memset(lr.pBits, 0, lr.Pitch * cell);
    int x = 0;
    for (int c = 0; c < GF_N; c++) {
        int w = g_gfAdv[c] + 2 * GF_M;
        SetRect(&g_gfSrc[c], x, 0, x + w, cell);
        gf_raster(g_gf.seg[c], g_gf.nseg[c], s, (float)GF_M, GF_M + g_gf.ascent * s, w, cell, (BYTE *)lr.pBits + x * 4, lr.Pitch);
        x += w + 2;
    }
    IDirect3DTexture9_UnlockRect(g_gfTex, 0);
    g_gfCell = (int)(px + 0.999f);
    logmsg("game font: digits drawn at %d px", px);
    return 1;
}
static int gf_width(const char *t) {
    int w = 0;
    for (; *t; t++) { const char *c = strchr(GF_CHARS, *t); w += c ? g_gfAdv[c - GF_CHARS] : g_gfAdv[0] / 2; }
    return w;
}
// draw t in rect r (DT_LEFT/DT_CENTER/DT_RIGHT, DT_VCENTER) with the sprite (between Begin and End)
static void gf_draw(ID3DXSprite *sp, const char *t, const RECT *r, DWORD fmt, DWORD color) {
    int w = gf_width(t);
    int x = fmt & DT_RIGHT ? r->right - w : fmt & DT_CENTER ? (r->left + r->right - w) / 2 : r->left;
    int y = fmt & DT_VCENTER ? (r->top + r->bottom - g_gfCell) / 2 : r->top;
    for (; *t; t++) {
        const char *c = strchr(GF_CHARS, *t);
        if (!c) { x += g_gfAdv[0] / 2; continue; }
        int i = c - GF_CHARS;
        float pos[3] = { (float)(x - GF_M), (float)(y - GF_M), 0.0f };
        sp->lpVtbl->Draw(sp, g_gfTex, &g_gfSrc[i], NULL, pos, color);
        x += g_gfAdv[i];
    }
}

// rundll32 MatchTimer.dll,GameFontTest <game folder>: reads the font as in the game and writes gamefont_test.pgm
// (the digits at 48 px) next to the DLL, to check the font outside the game
__declspec(dllexport) void CALLBACK GameFontTest(HWND hw, HINSTANCE hi, LPSTR cmd, int show) {
    wchar_t dir[MAX_PATH], out[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, cmd, -1, dir, MAX_PATH);
    for (size_t n = wcslen(dir); n && dir[n - 1] == L' '; ) dir[--n] = 0;          // rundll32 adds a trailing space
    if (!gf_load(GAME_FONT, dir)) return;
    float s = 48 / (g_gf.ascent + g_gf.descent);
    int cell = 48 + 2 * GF_M, tw = 0, x = 0;
    for (int c = 0; c < GF_N; c++) tw += (int)(g_gf.adv[c] * s + 0.5f) + 2 * GF_M;
    BYTE *rgba = calloc(tw * cell, 4), *grey = malloc(tw * cell);
    if (!rgba || !grey) return;
    for (int c = 0; c < GF_N; c++) {
        int w = (int)(g_gf.adv[c] * s + 0.5f) + 2 * GF_M;
        gf_raster(g_gf.seg[c], g_gf.nseg[c], s, (float)GF_M, GF_M + g_gf.ascent * s, w, cell, rgba + x * 4, tw * 4);
        x += w;
    }
    for (int i = 0; i < tw * cell; i++) grey[i] = rgba[i * 4 + 3];
    wcscpy(out, g_ini); wchar_t *sl = wcsrchr(out, L'\\'); if (sl) sl[1] = 0; else out[0] = 0;
    wcscat(out, L"gamefont_test.pgm");
    FILE *f = _wfopen(out, L"wb");
    if (f) { fprintf(f, "P5 %d %d 255\n", tw, cell); fwrite(grey, 1, tw * cell, f); fclose(f); }
    logmsg("GameFontTest: %d x %d written", tw, cell);
    free(rgba); free(grey);
}
