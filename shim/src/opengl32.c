/* opengl32.dll: wgl*, the GDI pixel-format calls and the gl* functions that need
 * more than passing their arguments on (the rest: opengl32_gen.c).
 *
 * GL runs on the game thread, straight into the host context: guest pointers
 * (client vertex arrays included) are host pointers into guest memory, which
 * stays put. */
#include "opengl32.h"

#include "platform.h"

#include <stdio.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include <stdlib.h>
#include <strings.h>

#include <SDL.h>

#include "pad_input.h"
#if defined(__SWITCH__)
#include "gamma_switch.h"
#endif

void user32_window_size(int *w, int *h);

/* ---- entry points ------------------------------------------------------- */

/* ---- hand-written gl* --------------------------------------------------- */

/* Extensions the game is told about: the ones its renderer can use here. */
static const char *const gl_extensions =
    "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_ARB_texture_env_add GL_EXT_texture_env_add "
    "GL_ARB_texture_env_combine GL_EXT_texture_filter_anisotropic GL_ARB_texture_compression "
    "GL_EXT_texture_compression_s3tc GL_EXT_draw_range_elements GL_ARB_texture_cube_map";

/* glGetString returns a pointer the guest reads: a copy in guest memory per name. */
void glm_glGetString(Ctx *c)
{
    static uint32_t cache[8];
    static const GLenum names[] = { GL_VENDOR, GL_RENDERER, GL_VERSION, GL_EXTENSIONS };
    GLenum e = (GLenum)ARG(0);
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        if (names[i] != e)
            continue;
        if (!cache[i]) {
            const char *s = e == GL_EXTENSIONS ? gl_extensions : (const char *)glGetString(e);
            if (e == GL_EXTENSIONS && getenv("COD_NO_S3TC"))
                s = "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_ARB_texture_env_add GL_EXT_texture_env_add "
                    "GL_ARB_texture_env_combine GL_EXT_draw_range_elements GL_ARB_texture_cube_map";
            if (e == GL_VERSION)
                s = "1.3.0";
            cache[i] = scratch_strdup(s ? s : "");
            rt_log("glGetString(%04X) = %s", e, s ? s : "");
        }
        r = cache[i];
    }
    stdcall_ret(c, r, 1);
}

/* The game sizes viewports for its resolution; the drawable may be bigger (HiDPI
 * or a scaled window). */
static void to_drawable(GLint *x, GLint *y, GLsizei *w, GLsizei *h)
{
    int mw, mh, dw, dh;
    platform_get_video_mode(&mw, &mh);
    platform_get_drawable_size(&dw, &dh);
    if (mw <= 0 || mh <= 0 || (mw == dw && mh == dh))
        return;
    *x = (GLint)((int64_t)*x * dw / mw);
    *y = (GLint)((int64_t)*y * dh / mh);
    *w = (GLsizei)((int64_t)*w * dw / mw);
    *h = (GLsizei)((int64_t)*h * dh / mh);
}

void glm_glViewport(Ctx *c)
{
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    to_drawable(&x, &y, &w, &h);
    glViewport(x, y, w, h);
    stdcall_ret(c, c->r[0], 4);
}

void glm_glScissor(Ctx *c)
{
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1);
    GLsizei w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    to_drawable(&x, &y, &w, &h);
    glScissor(x, y, w, h);
    stdcall_ret(c, c->r[0], 4);
}

/* Screenshots read the game's resolution: read the drawable and scale down. */
void glm_glReadPixels(Ctx *c)
{
    glReadPixels((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), (GLenum)ARG(4), (GLenum)ARG(5),
                 gptr(ARG(6)));
    stdcall_ret(c, c->r[0], 7);
}

/* ---- textures ---------------------------------------------------------------
 * The Vita gets no compressed textures: compressed uploads to vitaGL corrupted
 * memory while a level loaded (as in the author's Freedom Fighters port). DXT
 * images are decoded to RGBA here, and a compressed internal format asked of
 * glTexImage2D becomes the plain one. Elsewhere the driver takes them as they are. */

#define GL_DXT1_RGB 0x83F0u
#define GL_DXT1_RGBA 0x83F1u
#define GL_DXT3 0x83F2u
#define GL_DXT5 0x83F3u

typedef void (*pfn_teximage)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*pfn_ctex)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (*pfn_csubtex)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);

static void *gl_host(const char *name)
{
    void *p = platform_gl_proc(name);
    if (!p) {
        size_t n = strlen(name);
        char core[96];
        if (n > 3 && n < sizeof(core)) {
            snprintf(core, sizeof(core), "%.*s", (int)(n - 3), name);
            p = platform_gl_proc(core);
        }
    }
    return p;
}

static uint32_t rgb565_rgba(uint32_t c)
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}

static uint32_t mix_rgba(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t d)
{
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8)
        out |= ((((a >> sh) & 0xFFu) * wa + ((b >> sh) & 0xFFu) * wb) / d) << sh;
    return out;
}

/* The 16 colors of a DXT color block (4 bytes of endpoints, 4 of indices). */
static void dxt_colors(const uint8_t *b, int dxt1, uint32_t *px)
{
    uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8), c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    uint32_t pal[4];
    pal[0] = rgb565_rgba(c0);
    pal[1] = rgb565_rgba(c1);
    if (c0 > c1 || !dxt1) {
        pal[2] = mix_rgba(pal[0], pal[1], 2, 1, 3);
        pal[3] = mix_rgba(pal[0], pal[1], 1, 2, 3);
    } else {
        pal[2] = mix_rgba(pal[0], pal[1], 1, 1, 2);
        pal[3] = 0;                      /* transparent black */
    }
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++)
        px[i] = pal[(bits >> (2 * i)) & 3u];
}

static void dxt5_alpha(const uint8_t *b, uint32_t *px)
{
    uint32_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; i++)
            a[i + 1] = ((7u - (uint32_t)i) * a[0] + (uint32_t)i * a[1]) / 7u;
    } else {
        for (int i = 1; i < 5; i++)
            a[i + 1] = ((5u - (uint32_t)i) * a[0] + (uint32_t)i * a[1]) / 5u;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
        px[i] = (px[i] & 0x00FFFFFFu) | (a[(bits >> (3 * i)) & 7u] << 24);
}

/* A w x h DXT image (format fmt) to RGBA8, or NULL for another format. */
static __attribute__((unused)) uint32_t *dxt_decode(GLenum fmt, const uint8_t *src, uint32_t w, uint32_t h)
{
    if (fmt < GL_DXT1_RGB || fmt > GL_DXT5 || !src || !w || !h)
        return NULL;
    uint32_t *out = malloc((size_t)w * h * 4u);
    if (!out)
        return NULL;
    uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
    uint32_t bsize = fmt <= GL_DXT1_RGBA ? 8u : 16u;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + (by * bw + bx) * bsize;
            uint32_t px[16];
            if (bsize == 8u) {
                dxt_colors(b, 1, px);
            } else {
                dxt_colors(b + 8, 0, px);
                if (fmt == GL_DXT3) {
                    for (int i = 0; i < 16; i++) {
                        uint32_t a4 = (b[i / 2] >> (4 * (i & 1))) & 15u;
                        px[i] = (px[i] & 0x00FFFFFFu) | ((a4 * 17u) << 24);
                    }
                } else {
                    dxt5_alpha(b, px);
                }
            }
            for (uint32_t y = 0; y < 4u && by * 4u + y < h; y++)
                for (uint32_t x = 0; x < 4u && bx * 4u + x < w; x++)
                    out[(size_t)(by * 4u + y) * w + bx * 4u + x] = px[y * 4u + x];
        }
    }
    return out;
}

#if defined(__vita__)
static GLint plain_format(GLint f)
{
    switch ((uint32_t)f) {
    case GL_DXT1_RGB: case 0x84EDu: /* GL_COMPRESSED_RGB_ARB */ return GL_RGB;
    case GL_DXT1_RGBA: case GL_DXT3: case GL_DXT5: case 0x84EEu: /* GL_COMPRESSED_RGBA_ARB */ return GL_RGBA;
    default: return f;
    }
}
#endif

void glm_glTexImage2D(Ctx *c)
{
    static pfn_teximage fn;
    if (!fn)
        fn = (pfn_teximage)gl_host("glTexImage2D");
    GLint ifmt = (GLint)ARG(2);
#if defined(__vita__)
    ifmt = plain_format(ifmt);
#endif
    fn((GLenum)ARG(0), (GLint)ARG(1), ifmt, (GLsizei)ARG(3), (GLsizei)ARG(4), (GLint)ARG(5), (GLenum)ARG(6),
       (GLenum)ARG(7), gptr(ARG(8)));
    stdcall_ret(c, c->r[0], 9);
}

static void compressed_image(Ctx *c, const char *name)
{
    GLenum target = (GLenum)ARG(0), fmt = (GLenum)ARG(2);
    GLint level = (GLint)ARG(1), border = (GLint)ARG(5);
    GLsizei w = (GLsizei)ARG(3), h = (GLsizei)ARG(4), size = (GLsizei)ARG(6);
    const void *data = gptr(ARG(7));
#if defined(__vita__)
    (void)name, (void)size;
    uint32_t *rgba = dxt_decode(fmt, data, (uint32_t)w, (uint32_t)h);
    if (rgba) {
        glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    } else {
        rt_log("glCompressedTexImage2D: format %04X not decoded", fmt);
    }
#else
    static pfn_ctex fn;
    if (!fn)
        fn = (pfn_ctex)gl_host(name);
    fn(target, level, fmt, w, h, border, size, data);
#endif
    stdcall_ret(c, c->r[0], 8);
}

static void compressed_subimage(Ctx *c, const char *name)
{
    GLenum target = (GLenum)ARG(0), fmt = (GLenum)ARG(6);
    GLint level = (GLint)ARG(1), x = (GLint)ARG(2), y = (GLint)ARG(3);
    GLsizei w = (GLsizei)ARG(4), h = (GLsizei)ARG(5), size = (GLsizei)ARG(7);
    const void *data = gptr(ARG(8));
#if defined(__vita__)
    (void)name, (void)size;
    uint32_t *rgba = dxt_decode(fmt, data, (uint32_t)w, (uint32_t)h);
    if (rgba) {
        glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    }
#else
    static pfn_csubtex fn;
    if (!fn)
        fn = (pfn_csubtex)gl_host(name);
    fn(target, level, x, y, w, h, fmt, size, data);
#endif
    stdcall_ret(c, c->r[0], 9);
}

void glm_glCompressedTexImage2D(Ctx *c) { compressed_image(c, "glCompressedTexImage2D"); }
void glm_glCompressedTexImage2DARB(Ctx *c) { compressed_image(c, "glCompressedTexImage2DARB"); }
void glm_glCompressedTexSubImage2D(Ctx *c) { compressed_subimage(c, "glCompressedTexSubImage2D"); }
void glm_glCompressedTexSubImage2DARB(Ctx *c) { compressed_subimage(c, "glCompressedTexSubImage2DARB"); }

/* ---- client arrays and ranged draws ------------------------------------------
 * The game draws ranges of large shared vertex arrays with glDrawRangeElements.
 * vitaGL copies the client arrays from vertex 0 to `end` for every draw, so here
 * the arrays are moved to start at `start` (and the indices lowered by it) and
 * only the range is copied. The pointers are tracked for that and put back after. */

typedef struct ClientArray {
    GLint size;
    GLenum type;
    GLsizei stride;
    uint32_t ptr;        /* guest address (0: none set) */
} ClientArray;

#define MAX_TEXUNITS 8
static ClientArray va_vertex, va_color, va_normal, va_tex[MAX_TEXUNITS];
static unsigned va_client_unit;

static uint32_t type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}

static uint32_t array_stride(const ClientArray *a)
{
    return a->stride ? (uint32_t)a->stride : (uint32_t)a->size * type_size(a->type);
}

static void set_vertex(const ClientArray *a, uint32_t off) { glVertexPointer(a->size, a->type, a->stride, gptr(a->ptr + off)); }
static void set_color(const ClientArray *a, uint32_t off) { glColorPointer(a->size, a->type, a->stride, gptr(a->ptr + off)); }
static void set_normal(const ClientArray *a, uint32_t off) { glNormalPointer(a->type, a->stride, gptr(a->ptr + off)); }
static void set_tex(const ClientArray *a, uint32_t off) { glTexCoordPointer(a->size, a->type, a->stride, gptr(a->ptr + off)); }

void glm_glVertexPointer(Ctx *c)
{
    va_vertex = (ClientArray){ (GLint)ARG(0), (GLenum)ARG(1), (GLsizei)ARG(2), ARG(3) };
    set_vertex(&va_vertex, 0);
    stdcall_ret(c, c->r[0], 4);
}

void glm_glColorPointer(Ctx *c)
{
    va_color = (ClientArray){ (GLint)ARG(0), (GLenum)ARG(1), (GLsizei)ARG(2), ARG(3) };
    set_color(&va_color, 0);
    stdcall_ret(c, c->r[0], 4);
}

void glm_glNormalPointer(Ctx *c)
{
    va_normal = (ClientArray){ 3, (GLenum)ARG(0), (GLsizei)ARG(1), ARG(2) };
    set_normal(&va_normal, 0);
    stdcall_ret(c, c->r[0], 3);
}

void glm_glTexCoordPointer(Ctx *c)
{
    if (va_client_unit < MAX_TEXUNITS)
        va_tex[va_client_unit] = (ClientArray){ (GLint)ARG(0), (GLenum)ARG(1), (GLsizei)ARG(2), ARG(3) };
    glTexCoordPointer((GLint)ARG(0), (GLenum)ARG(1), (GLsizei)ARG(2), gptr(ARG(3)));
    stdcall_ret(c, c->r[0], 4);
}

static void client_active(Ctx *c)
{
    GLenum unit = (GLenum)ARG(0);
    va_client_unit = unit - GL_TEXTURE0;
    glClientActiveTexture(unit);
    stdcall_ret(c, c->r[0], 1);
}

void glm_glClientActiveTexture(Ctx *c) { client_active(c); }
void glm_glClientActiveTextureARB(Ctx *c) { client_active(c); }

/* Moves every set array by `first` vertices (0: back where the game put them). */
static void shift_arrays(uint32_t first)
{
    if (va_vertex.ptr)
        set_vertex(&va_vertex, first * array_stride(&va_vertex));
    if (va_color.ptr)
        set_color(&va_color, first * array_stride(&va_color));
    if (va_normal.ptr)
        set_normal(&va_normal, first * array_stride(&va_normal));
    for (unsigned u = 0; u < MAX_TEXUNITS; u++) {
        if (!va_tex[u].ptr)
            continue;
        glClientActiveTexture(GL_TEXTURE0 + u);
        set_tex(&va_tex[u], first * array_stride(&va_tex[u]));
    }
    glClientActiveTexture(GL_TEXTURE0 + va_client_unit);
}

/* Element size in bytes of a client array (what one vertex holds). */
static uint32_t elem_size(const ClientArray *a) { return (uint32_t)a->size * type_size(a->type); }

/* Sparse draws (a few triangles indexing a range of thousands of vertices, as the
 * world's surfaces do): the referenced vertices are gathered into tight temporary
 * arrays and the indices renumbered, so vitaGL copies what is drawn only. Only when
 * the range is far larger than the draw: vitaGL's contiguous copy is cheaper per
 * vertex than this gather. */
static int draw_compacted(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, uint32_t idx)
{
    static uint32_t *stamp, *remap, stamp_cap, gen;
    static uint16_t *out_idx;
    static uint32_t *order, idx_cap;
    static uint8_t *bufs[3 + MAX_TEXUNITS];
    static uint32_t buf_cap[3 + MAX_TEXUNITS];
    uint32_t range = end - start + 1u;
    if (range > stamp_cap) {
        uint32_t *ns = realloc(stamp, range * sizeof(uint32_t)), *nr = ns ? realloc(remap, range * sizeof(uint32_t)) : NULL;
        if (!ns || !nr) {
            stamp = ns;
            return 0;
        }
        memset(ns + stamp_cap, 0, (range - stamp_cap) * sizeof(uint32_t));
        stamp = ns;
        remap = nr;
        stamp_cap = range;
    }
    if ((uint32_t)count > idx_cap) {
        idx_cap = (uint32_t)count * 2u;
        out_idx = realloc(out_idx, idx_cap * sizeof(uint16_t));
        order = realloc(order, idx_cap * sizeof(uint32_t));
        if (!out_idx || !order) {
            idx_cap = 0;
            return 0;
        }
    }
    if (++gen == 0) {
        memset(stamp, 0, stamp_cap * sizeof(uint32_t));
        gen = 1;
    }
    uint32_t n = 0;
    for (GLsizei i = 0; i < count; i++) {
        uint32_t v = (type == GL_UNSIGNED_SHORT ? RD16(idx + 2u * (uint32_t)i) : RD32(idx + 4u * (uint32_t)i)) - start;
        if (v >= range)
            return 0;
        if (stamp[v] != gen) {
            stamp[v] = gen;
            remap[v] = n;
            order[n++] = v + start;
        }
        out_idx[i] = (uint16_t)remap[v];
    }
    if (n > 65535u)
        return 0;
    /* Gather every set array into its own tight buffer. */
    ClientArray *arrays[3 + MAX_TEXUNITS] = { &va_vertex, &va_color, &va_normal };
    for (unsigned u = 0; u < MAX_TEXUNITS; u++)
        arrays[3 + u] = &va_tex[u];
    for (unsigned k = 0; k < 3 + MAX_TEXUNITS; k++) {
        ClientArray *a = arrays[k];
        if (!a->ptr)
            continue;
        uint32_t es = elem_size(a), st = array_stride(a);
        if (n * es > buf_cap[k]) {
            buf_cap[k] = n * es * 2u;
            bufs[k] = realloc(bufs[k], buf_cap[k]);
            if (!bufs[k]) {
                buf_cap[k] = 0;
                return 0;
            }
        }
        uint8_t *dst = bufs[k];
        const uint8_t *src = g_mem + a->ptr;
        if ((es & 3u) == 0 && (st & 3u) == 0 && (a->ptr & 3u) == 0) {
            /* word copies inline: a library call per vertex costs more than the copy */
            uint32_t w = es / 4u, sw = st / 4u;
            uint32_t *d32 = (uint32_t *)(void *)dst;
            const uint32_t *s32 = (const uint32_t *)(const void *)src;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t *sv = s32 + order[i] * sw;
                for (uint32_t k2 = 0; k2 < w; k2++)
                    *d32++ = sv[k2];
            }
        } else {
            for (uint32_t i = 0; i < n; i++)
                memcpy(dst + i * es, src + order[i] * st, es);
        }
    }
    if (va_vertex.ptr)
        glVertexPointer(va_vertex.size, va_vertex.type, 0, bufs[0]);
    if (va_color.ptr)
        glColorPointer(va_color.size, va_color.type, 0, bufs[1]);
    if (va_normal.ptr)
        glNormalPointer(va_normal.type, 0, bufs[2]);
    for (unsigned u = 0; u < MAX_TEXUNITS; u++) {
        if (!va_tex[u].ptr)
            continue;
        glClientActiveTexture(GL_TEXTURE0 + u);
        glTexCoordPointer(va_tex[u].size, va_tex[u].type, 0, bufs[3 + u]);
    }
    glClientActiveTexture(GL_TEXTURE0 + va_client_unit);
    glDrawRangeElements(mode, 0, n - 1u, count, GL_UNSIGNED_SHORT, out_idx);
    shift_arrays(0);
    return 1;
}

/* out[i] = in[i] - start, from 16- or 32-bit indices (the game's ~110k per frame). */
static void rebase_indices(uint16_t *out, const uint8_t *in, uint32_t n, int in32, uint32_t start)
{
    uint32_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (in32) {
        uint32x4_t s = vdupq_n_u32(start);
        for (; i + 8u <= n; i += 8u) {
            uint32x4_t a = vsubq_u32(vld1q_u32((const uint32_t *)(const void *)(in + 4u * i)), s);
            uint32x4_t b = vsubq_u32(vld1q_u32((const uint32_t *)(const void *)(in + 4u * i + 16u)), s);
            vst1q_u16(out + i, vcombine_u16(vmovn_u32(a), vmovn_u32(b)));
        }
    } else {
        uint16x8_t s = vdupq_n_u16((uint16_t)start);
        for (; i + 8u <= n; i += 8u)
            vst1q_u16(out + i, vsubq_u16(vld1q_u16((const uint16_t *)(const void *)(in + 2u * i)), s));
    }
#endif
    if (in32) {
        for (; i < n; i++) {
            uint32_t v;
            memcpy(&v, in + 4u * i, 4);
            out[i] = (uint16_t)(v - start);
        }
    } else {
        for (; i < n; i++) {
            uint16_t v;
            memcpy(&v, in + 2u * i, 2);
            out[i] = (uint16_t)(v - start);
        }
    }
}

/* COD_DRAW_STATS: per-frame totals of the ranged draws (logged every 100 frames). */
static int draw_stats = -1;
static uint64_t ds_up_n, ds_up_bytes, ds_evict;
static uint64_t ds_draws, ds_range, ds_count, ds_bytes, ds_big, ds_vbo, ds_normal, ds_low, ds_idx_hunk;
static uint32_t ds_frames;
static uint32_t ds_ptrs[16];
static unsigned ds_nptrs;

static void draw_stats_frame(void);

/* ---- static geometry in buffer objects ------------------------------------------
 * The world's vertex arrays live in the game's hunk and do not change while a
 * level is played, yet every frame drew ~17 MB of them, which vitaGL copies to
 * GPU memory per draw. Each interleaved vertex block in the hunk gets one buffer
 * object holding the union of the ranges drawn from it (looking around must not
 * multiply copies: one buffer per range ran vitaGL out of memory), with a shadow
 * copy: every draw compares 16 words of its range and uploads the range again if
 * they changed (a new level reuses the hunk); a block that keeps changing is
 * dynamic and drawn from client memory. Arrays inside the executable's image (the
 * tess arrays, rewritten every frame) are never cached. */

typedef struct VboEntry {
    uint32_t ptr, stride, esize;   /* key: the array (one buffer per array) */
    uint32_t lo, hi;               /* vertices held: lo..hi */
    GLuint buf;
    uint8_t *shadow;               /* what the buffer holds, to spot changes */
    uint32_t changes;              /* content changes seen; past a few the array is dynamic */
    int dynamic;
    uint32_t last_use;
    uint32_t bytes;                /* held in the buffer (and the shadow) */
    int32_t next;                  /* hash chain (-1: end) */
} VboEntry;

/* A level draws from far more arrays than a few dozen (every model has its own),
 * and an evicted array costs a full upload when drawn again: 64 slots made the
 * first mission upload ~6 MB a frame. Entries are found through a hash of the
 * pointer; past VBO_BUDGET bytes the least recently drawn array not drawn this
 * frame is dropped. */
#define VBO_SLOTS 1024u
#define VBO_BUCKETS 2048u
#define VBO_BUDGET (40u << 20)
#define VBO_MAX_BYTES (16u << 20)  /* larger arrays are not cached */
#define VBO_DYNAMIC_AFTER 4u
static VboEntry vbo_cache[VBO_SLOTS];
static int32_t vbo_bucket[VBO_BUCKETS];
static uint32_t vbo_used_slots, vbo_total;
static uint32_t vbo_frame;
static int vbo_on = -1;

static uint32_t vbo_hash(uint32_t ptr) { return (ptr * 2654435761u) >> (32u - 11u); }

static int vbo_upload(VboEntry *e)
{
    uint32_t base = e->ptr + e->lo * e->stride, bytes = (e->hi - e->lo) * e->stride + e->esize;
    uint8_t *sh = realloc(e->shadow, bytes);
    if (!sh)
        return 0;
    e->shadow = sh;
    vbo_total += bytes - e->bytes;
    e->bytes = bytes;
    ds_up_n++;
    ds_up_bytes += bytes;
    memcpy(sh, gptr(base), bytes);
    glBindBuffer(GL_ARRAY_BUFFER, e->buf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizei)bytes, sh, GL_STATIC_DRAW);
    return 1;
}

/* Whether the guest bytes of start..end still equal the shadow (16 words spread
 * over the range, its ends included); a changed range is uploaded again. */
static int vbo_refresh(VboEntry *e, uint32_t start, uint32_t end)
{
    uint32_t off = (start - e->lo) * e->stride, len = (end - start) * e->stride + e->esize;
    const uint8_t *g = gptr(e->ptr + start * e->stride);
    const uint8_t *h = e->shadow + off;
    uint32_t words = len / 4u;
    int same = 1;
    for (uint32_t k = 0; k < 16u && same; k++) {
        uint32_t w = words > 1u ? (uint32_t)((uint64_t)k * (words - 1u) / 15u) : 0u;
        same = memcmp(g + 4u * w, h + 4u * w, 4) == 0;
    }
    if (same)
        return 1;
    if (++e->changes >= VBO_DYNAMIC_AFTER) {
        e->dynamic = 1;   /* rewritten while played: drawn from client memory from now on */
        return 0;
    }
    memcpy(e->shadow + off, g, len);
    ds_up_n++;
    ds_up_bytes += len;
    glBindBuffer(GL_ARRAY_BUFFER, e->buf);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)off, (GLsizeiptr)len, e->shadow + off);
    return 1;
}

static void vbo_unlink(VboEntry *e)
{
    int32_t *link = &vbo_bucket[vbo_hash(e->ptr)];
    int32_t idx = (int32_t)(e - vbo_cache);
    while (*link >= 0 && *link != idx)
        link = &vbo_cache[*link].next;
    if (*link == idx)
        *link = e->next;
}

/* The least recently drawn entry not drawn this frame (NULL: none). */
static VboEntry *vbo_lru(void)
{
    VboEntry *v = NULL;
    for (uint32_t i = 0; i < vbo_used_slots; i++) {
        VboEntry *x = &vbo_cache[i];
        if (x->ptr && x->last_use != vbo_frame && (!v || x->last_use < v->last_use))
            v = x;
    }
    return v;
}

static void vbo_drop(VboEntry *e)
{
    ds_evict++;
    vbo_unlink(e);
    vbo_total -= e->bytes;
    e->bytes = 0;
    free(e->shadow);
    e->shadow = NULL;
    e->ptr = 0;
    if (e->buf) {   /* its memory back (vitaGL frees it once the GPU is done with it) */
        glDeleteBuffers(1, &e->buf);
        e->buf = 0;
    }
}

/* Room for `bytes` more under the budget, dropping old entries. */
static void vbo_make_room(uint32_t bytes)
{
    while (vbo_total + bytes > VBO_BUDGET) {
        VboEntry *v = vbo_lru();
        if (!v)
            break;
        vbo_drop(v);
    }
}

/* Vertices to hold when an array's held range lo..hi must take start..end: the
 * union, widened by half of it on the side(s) that grew, so a camera turning
 * over a large array uploads it a few times instead of at every new piece. */
static void vbo_grow(uint32_t *lo, uint32_t *hi, uint32_t start, uint32_t end, uint32_t st, uint32_t es)
{
    uint32_t nlo = start < *lo ? start : *lo, nhi = end > *hi ? end : *hi;
    uint32_t slack = (nhi - nlo) / 2u + 64u;
    if (start < *lo)
        nlo = nlo > slack ? nlo - slack : 0u;
    if (end > *hi)
        nhi += slack;
    while (nhi > nlo && (nhi - nlo) * st + es > VBO_MAX_BYTES) {   /* keep the drawn part */
        if (nlo < start)
            nlo++;
        else if (nhi > end)
            nhi--;
        else
            break;
    }
    *lo = nlo;
    *hi = nhi;
}

/* Buffer holding the array's vertices start..end (one buffer per array: the
 * union of the ranges drawn from it, so looking around does not multiply the
 * copies), with *offset the byte offset of `start` in it; 0 when not cached
 * (not in the hunk, too big, or found to be rewritten during play). */
static GLuint vbo_block(uint32_t ptr, uint32_t st, uint32_t es, uint32_t start, uint32_t end, uint32_t *offset)
{
    if (!ptr || ptr < GUEST_HEAP_BASE || st < 4u || es < 4u)
        return 0;
    if (!vbo_used_slots)
        memset(vbo_bucket, 0xFF, sizeof(vbo_bucket));
    VboEntry *e = NULL;
    for (int32_t i = vbo_bucket[vbo_hash(ptr)]; i >= 0; i = vbo_cache[i].next) {
        VboEntry *x = &vbo_cache[i];
        if (x->ptr == ptr && x->stride == st && x->esize == es) {
            e = x;
            break;
        }
    }
    if (e && e->dynamic) {
        e->last_use = vbo_frame;
        return 0;
    }
    if ((end - start) * st + es > VBO_MAX_BYTES)
        return 0;
    if (!e) {
        uint32_t lo = start, hi = end;
        vbo_grow(&lo, &hi, start, end, st, es);
        uint32_t bytes = (hi - lo) * st + es;
        vbo_make_room(bytes);
        if (vbo_total + bytes > VBO_BUDGET)
            return 0;
        /* a free slot: never used, or dropped */
        for (uint32_t i = 0; i < vbo_used_slots && !e; i++)
            if (!vbo_cache[i].ptr)
                e = &vbo_cache[i];
        if (!e && vbo_used_slots < VBO_SLOTS)
            e = &vbo_cache[vbo_used_slots++];
        if (!e) {
            e = vbo_lru();
            if (!e)
                return 0;
            vbo_drop(e);
        }
        if (!e->buf)
            glGenBuffers(1, &e->buf);
        e->ptr = ptr;
        e->stride = st;
        e->esize = es;
        e->lo = lo;
        e->hi = hi;
        e->changes = 0;
        e->dynamic = 0;
        e->bytes = 0;
        uint32_t h = vbo_hash(ptr);
        e->next = vbo_bucket[h];
        vbo_bucket[h] = (int32_t)(e - vbo_cache);
        if (!vbo_upload(e)) {
            vbo_drop(e);
            return 0;
        }
    } else if (start < e->lo || end > e->hi) {
        uint32_t lo = e->lo, hi = e->hi;
        vbo_grow(&lo, &hi, start, end, st, es);
        e->last_use = vbo_frame;   /* not a victim of its own growth */
        vbo_make_room((hi - lo) * st + es - e->bytes);
        e->lo = lo;
        e->hi = hi;
        if (!vbo_upload(e)) {
            vbo_drop(e);
            return 0;
        }
    } else if (!vbo_refresh(e, start, end)) {
        e->last_use = vbo_frame;
        return 0;
    }
    e->last_use = vbo_frame;
    *offset = (start - e->lo) * st;
    return e->buf;
}

static void draw_stats_frame(void)
{
    if (draw_stats <= 0)
        return;
    if (++ds_frames % 100u == 0) {
        rt_log("draws/frame %.0f, range verts/frame %.0f, indices/frame %.0f, array bytes/frame %.0f KB, "
               "index arrays in the hunk %.0f, draws with range > 4x indices %.0f, from buffer objects %.0f (not: normals %.0f, image arrays %.0f); vertex arrays at:",
               ds_draws / 100.0, ds_range / 100.0, ds_count / 100.0, ds_bytes / 102400.0, ds_idx_hunk / 100.0, ds_big / 100.0, ds_vbo / 100.0,
               ds_normal / 100.0, ds_low / 100.0);
        rt_log("buffer uploads/frame %.1f (%.0f KB), cache evictions/frame %.1f", ds_up_n / 100.0,
               ds_up_bytes / 102400.0, ds_evict / 100.0);
        ds_up_n = ds_up_bytes = ds_evict = 0;
        rt_log("buffer cache: %u entries, %u KB", vbo_used_slots, vbo_total / 1024u);
        for (unsigned i = 0; i < vbo_used_slots && draw_stats > 1; i++)
            if (vbo_cache[i].ptr)
                rt_log("  vbo %08X stride %u elem %u verts %u..%u changes %u%s", vbo_cache[i].ptr, vbo_cache[i].stride,
                       vbo_cache[i].esize, vbo_cache[i].lo, vbo_cache[i].hi, vbo_cache[i].changes,
                       vbo_cache[i].dynamic ? " dynamic" : "");
        ds_draws = ds_range = ds_count = ds_bytes = ds_big = ds_vbo = ds_normal = ds_low = ds_idx_hunk = 0;
        ds_nptrs = 0;
    }
}

/* Profiler phase of the ranged draw (COD_PROFILE builds; ids named in profile.c). */
#ifdef COD_PROFILE
/* The drawing thread's profiler slot (the render thread's, or the game thread's;
 * GL is never used by both at once). */
static volatile uint32_t *draw_phase_slot;
#define DRAW_PHASE(id) (*draw_phase_slot = (id))
#else
#define DRAW_PHASE(id) ((void)0)
#endif

/* Draws start..end with every hunk array from a buffer object; 0 if some array
 * cannot be (then the caller draws from client memory). */
static int draw_from_vbos(GLenum mode, uint32_t start, uint32_t end, GLsizei count, const uint16_t *idx16)
{
    if (vbo_on < 0) {
        const char *e = getenv("COD_VBO_CACHE");
#if defined(__vita__)
        vbo_on = !(e && e[0] == '0');
#else
        vbo_on = e && e[0] == '1';
#endif
    }
    if (!vbo_on || !va_vertex.ptr || va_vertex.ptr < GUEST_HEAP_BASE) {
        ds_normal += 0;
        ds_low += va_vertex.ptr && va_vertex.ptr < GUEST_HEAP_BASE;
        return 0;
    }
    ds_vbo++;
    /* The draw's arrays are one interleaved block (the vertex array's stride, all
     * within one stride of each other): the block goes into one buffer and every
     * array of it points into that buffer; arrays outside the block stay client
     * arrays. */
    uint32_t st = array_stride(&va_vertex);
    ClientArray *all[3 + MAX_TEXUNITS] = { &va_vertex, &va_color, &va_normal };
    for (unsigned u = 0; u < MAX_TEXUNITS; u++)
        all[3 + u] = &va_tex[u];
    uint32_t lo = va_vertex.ptr, hi = va_vertex.ptr + elem_size(&va_vertex);
    for (unsigned k = 1; k < 3 + MAX_TEXUNITS; k++) {
        ClientArray *x = all[k];
        if (x->ptr && array_stride(x) == st && x->ptr + st > va_vertex.ptr && x->ptr < va_vertex.ptr + st) {
            if (x->ptr < lo)
                lo = x->ptr;
            if (x->ptr + elem_size(x) > hi)
                hi = x->ptr + elem_size(x);
        }
    }
    if (hi - lo > st)
        return 0;
    uint32_t boff;
    DRAW_PHASE(PROF_H_BK_BUF);
    GLuint buf = vbo_block(lo, st, hi - lo, start, end, &boff);
    if (!buf)
        return 0;
    DRAW_PHASE(PROF_H_BK_PTRS);
    int in_block[3 + MAX_TEXUNITS];
    for (unsigned k = 0; k < 3 + MAX_TEXUNITS; k++) {
        ClientArray *x = all[k];
        in_block[k] = x->ptr && array_stride(x) == st && x->ptr >= lo && x->ptr + elem_size(x) <= hi;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glVertexPointer(va_vertex.size, va_vertex.type, va_vertex.stride, (const void *)(uintptr_t)(boff + va_vertex.ptr - lo));
    if (in_block[1])
        glColorPointer(va_color.size, va_color.type, va_color.stride, (const void *)(uintptr_t)(boff + va_color.ptr - lo));
    if (in_block[2])
        glNormalPointer(va_normal.type, va_normal.stride, (const void *)(uintptr_t)(boff + va_normal.ptr - lo));
    for (unsigned u = 0; u < MAX_TEXUNITS; u++) {
        if (!in_block[3 + u])
            continue;
        glClientActiveTexture(GL_TEXTURE0 + u);
        glTexCoordPointer(va_tex[u].size, va_tex[u].type, va_tex[u].stride, (const void *)(uintptr_t)(boff + va_tex[u].ptr - lo));
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (va_color.ptr && !in_block[1])
        set_color(&va_color, start * array_stride(&va_color));
    if (va_normal.ptr && !in_block[2])
        set_normal(&va_normal, start * array_stride(&va_normal));
    for (unsigned u = 0; u < MAX_TEXUNITS; u++) {
        if (!va_tex[u].ptr || in_block[3 + u])
            continue;
        glClientActiveTexture(GL_TEXTURE0 + u);
        set_tex(&va_tex[u], start * array_stride(&va_tex[u]));
    }
    glClientActiveTexture(GL_TEXTURE0 + va_client_unit);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    DRAW_PHASE(PROF_H_GLDRAW);
    glDrawRangeElements(mode, 0, end - start, count, GL_UNSIGNED_SHORT, idx16);
    DRAW_PHASE(PROF_H_BK_PTRS);
    shift_arrays(0);   /* the game's client pointers back (no buffer bound) */
    return 1;
}

static void draw_range(Ctx *c)
{
    GLenum mode = (GLenum)ARG(0), type = (GLenum)ARG(4);
    GLuint start = (GLuint)ARG(1), end = (GLuint)ARG(2);
    GLsizei count = (GLsizei)ARG(3);
    uint32_t idx = ARG(5);
    if (draw_stats < 0)
        draw_stats = getenv("COD_DRAW_STATS") ? atoi(getenv("COD_DRAW_STATS")) : 0;   /* 2: and the cache entries */
    if (draw_stats > 0 && end >= start) {
        uint32_t r = end - start + 1u;
        ds_draws++;
        ds_range += r;
        ds_count += (uint32_t)count;
        ds_big += r > 4u * (uint32_t)count;
        ds_idx_hunk += idx >= GUEST_HEAP_BASE;
        uint32_t per = (va_vertex.ptr ? array_stride(&va_vertex) : 0) + (va_color.ptr ? array_stride(&va_color) : 0);
        for (unsigned u = 0; u < MAX_TEXUNITS; u++)
            per += va_tex[u].ptr ? array_stride(&va_tex[u]) : 0;
        ds_bytes += (uint64_t)r * per;
        unsigned i;
        for (i = 0; i < ds_nptrs && ds_ptrs[i] != va_vertex.ptr; i++)
            ;
        if (i == ds_nptrs && ds_nptrs < 16)
            ds_ptrs[ds_nptrs++] = va_vertex.ptr;
    }
    int typed = type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT;
#ifdef COD_PROFILE
    draw_phase_slot = &c->prof->cur;
    const uint32_t prof_hprev = *draw_phase_slot;
#endif
    DRAW_PHASE(PROF_H_PACK);
    if (count > 0 && end >= start && typed && end - start + 1u > (uint32_t)count * 8u + 64u &&
        draw_compacted(mode, start, end, count, type, idx)) {
        DRAW_PHASE(prof_hprev);
        stdcall_ret(c, c->r[0], 6);
        return;
    }
    if (count <= 0 || end < start || !typed) {
        DRAW_PHASE(PROF_H_GLDRAW);
        glDrawRangeElements(mode, start, end, count, type, gptr(idx));
        DRAW_PHASE(prof_hprev);
        stdcall_ret(c, c->r[0], 6);
        return;
    }
    static uint16_t *tmp16;
    static uint32_t cap;
    if ((uint32_t)count > cap) {
        cap = (uint32_t)count * 2u;
        tmp16 = realloc(tmp16, cap * sizeof(uint16_t));
    }
    int fits16 = end - start < 65536u;
    if (fits16 && tmp16) {
        DRAW_PHASE(PROF_H_INDEX);
        rebase_indices(tmp16, g_mem + idx, (uint32_t)count, type == GL_UNSIGNED_INT, start);
        if (!draw_from_vbos(mode, start, end, count, tmp16)) {
            DRAW_PHASE(PROF_H_BK_PTRS);
            shift_arrays(start);
            DRAW_PHASE(PROF_H_GLDRAW);
            glDrawRangeElements(mode, 0, end - start, count, GL_UNSIGNED_SHORT, tmp16);
            DRAW_PHASE(PROF_H_BK_PTRS);
            shift_arrays(0);
        }
    } else {
        DRAW_PHASE(PROF_H_GLDRAW);
        glDrawRangeElements(mode, start, end, count, type, gptr(idx));
    }
    DRAW_PHASE(prof_hprev);
    stdcall_ret(c, c->r[0], 6);
}

void glm_glDrawRangeElements(Ctx *c) { draw_range(c); }
void glm_glDrawRangeElementsEXT(Ctx *c) { draw_range(c); }

/* Compiled vertex arrays are only a hint. */
void glm_glLockArraysEXT(Ctx *c) { stdcall_ret(c, c->r[0], 2); }
void glm_glUnlockArraysEXT(Ctx *c) { stdcall_ret(c, c->r[0], 0); }

/* ---- wgl ---------------------------------------------------------------- */

#define HGLRC_FAKE 0x00C0FFEEu

static void wgl_create_context(Ctx *c)
{
    int w, h;
    user32_window_size(&w, &h);
    platform_set_video_mode(w, h);
    platform_gl_init();
    rt_log("wglCreateContext: %dx%d", w, h);
    stdcall_ret(c, HGLRC_FAKE, 1);
}

static void wgl_make_current(Ctx *c)
{
    if (ARG(1))
        platform_gl_init();
    stdcall_ret(c, 1, 2);
}

static void wgl_delete_context(Ctx *c) { stdcall_ret(c, 1, 1); }
static void wgl_get_current_context(Ctx *c) { stdcall_ret(c, HGLRC_FAKE, 0); }
static void wgl_get_current_dc(Ctx *c) { stdcall_ret(c, 0x00DC0001u, 0); }
static void wgl_share_lists(Ctx *c) { stdcall_ret(c, 1, 2); }

static void wgl_get_proc_address(Ctx *c)
{
    const char *name = gstr(ARG(0));
    uint32_t r = name ? opengl32_get_proc(name) : 0;
    rt_log("wglGetProcAddress(%s) -> %08X", name ? name : "", r);
    stdcall_ret(c, r, 1);
}

static void present(void);

static void wgl_swap_buffers(Ctx *c)
{
    present();
    stdcall_ret(c, 1, 1);
}

/* Vsync stays on whatever the game asks (r_swapInterval 0 is its default): at
 * hundreds of frames a second its movement code, which counts whole milliseconds,
 * steps unevenly (crouching, view height). COD_VSYNC=0 turns it off. */
static void wgl_swap_interval(Ctx *c)
{
    const char *vs = getenv("COD_VSYNC");
    platform_set_swap_interval(!(vs && vs[0] == '0'));
    stdcall_ret(c, 1, 1);
}

static void wgl_get_swap_interval(Ctx *c) { stdcall_ret(c, 1, 0); }

static void wgl_get_extensions_string(Ctx *c)
{
    static uint32_t s;
    if (!s)
        s = scratch_strdup("WGL_EXT_swap_control WGL_EXT_extensions_string");
    stdcall_ret(c, s, 0);
}

static void wgl_get_extensions_string_arb(Ctx *c)
{
    static uint32_t s;
    if (!s)
        s = scratch_strdup("WGL_EXT_swap_control WGL_EXT_extensions_string");
    stdcall_ret(c, s, 1);
}

/* PIXELFORMATDESCRIPTOR of the one format offered: RGBA 32, depth 24, stencil 8,
 * double-buffered, accelerated. */
static void fill_pfd(uint32_t p, uint32_t size)
{
    if (!p || size < 40u)
        return;
    memset(gptr(p), 0, 40);
    WR16(p + 0u, 40);
    WR16(p + 2u, 1);
    WR32(p + 4u, 0x00000004u | 0x00000020u | 0x00000001u);   /* DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER */
    WR8(p + 8u, 0);    /* PFD_TYPE_RGBA */
    WR8(p + 9u, 32);   /* cColorBits */
    WR8(p + 10u, 8);
    WR8(p + 11u, 16);
    WR8(p + 12u, 8);
    WR8(p + 13u, 8);
    WR8(p + 14u, 8);
    WR8(p + 15u, 0);
    WR8(p + 16u, 8);   /* alpha */
    WR8(p + 17u, 24);
    WR8(p + 23u, 24);  /* cDepthBits */
    WR8(p + 24u, 8);   /* cStencilBits */
}

static void wgl_describe_pixel_format(Ctx *c)
{
    fill_pfd(ARG(3), ARG(2));
    stdcall_ret(c, 1, 4);
}

static void wgl_choose_pixel_format(Ctx *c) { stdcall_ret(c, 1, 2); }
static void wgl_set_pixel_format(Ctx *c) { stdcall_ret(c, 1, 3); }
static void wgl_get_pixel_format(Ctx *c) { stdcall_ret(c, 1, 1); }

static const struct {
    const char *name;
    GuestFn fn;
} wgl_entries[] = {
    { "wglCreateContext", wgl_create_context },
    { "wglMakeCurrent", wgl_make_current },
    { "wglDeleteContext", wgl_delete_context },
    { "wglGetCurrentContext", wgl_get_current_context },
    { "wglGetCurrentDC", wgl_get_current_dc },
    { "wglShareLists", wgl_share_lists },
    { "wglGetProcAddress", wgl_get_proc_address },
    { "wglSwapBuffers", wgl_swap_buffers },
    { "wglSwapIntervalEXT", wgl_swap_interval },
    { "wglGetSwapIntervalEXT", wgl_get_swap_interval },
    { "wglGetExtensionsStringEXT", wgl_get_extensions_string },
    { "wglGetExtensionsStringARB", wgl_get_extensions_string_arb },
    { "wglDescribePixelFormat", wgl_describe_pixel_format },
    { "wglChoosePixelFormat", wgl_choose_pixel_format },
    { "wglSetPixelFormat", wgl_set_pixel_format },
    { "wglGetPixelFormat", wgl_get_pixel_format },
};
static const unsigned wgl_entry_count = sizeof(wgl_entries) / sizeof(wgl_entries[0]);

static uint32_t gl_missing(void) { return 0; }

uint32_t opengl32_get_proc(const char *name)
{
    for (unsigned i = 0; i < wgl_entry_count; i++)
        if (strcmp(wgl_entries[i].name, name) == 0) {
            uint32_t t = thunk_register(wgl_entries[i].fn, wgl_entries[i].name);
            thunk_mark_gated(t);
            return t;
        }
    for (unsigned i = 0; i < g_gl_entry_count; i++) {
        const GlEntry *e = &g_gl_entries[i];
        if (strcmp(e->name, name) != 0)
            continue;
        if (e->host && !*e->host) {
            void *p = platform_gl_proc(name);
            size_t n = strlen(name);
            if (!p && n > 3 && (!strcmp(name + n - 3, "ARB") || !strcmp(name + n - 3, "EXT"))) {
                char core[96];
                snprintf(core, sizeof(core), "%.*s", (int)(n - 3), name);
                p = platform_gl_proc(core);
            }
            if (!p) {
                /* The game looks up the whole GL 1.1 set and calls some functions
                 * without checking (vitaGL lacks a few, e.g. glLightModeli): they do
                 * nothing. Calling a function without parameters with arguments is
                 * harmless in the host ABIs (the caller owns them). */
                rt_log("GL: %s not in the driver: does nothing", name);
                p = (void *)gl_missing;
            }
            *e->host = p;
        }
        uint32_t t = thunk_register(e->fn, e->name);
        thunk_mark_gated(t);   /* GL from the game thread waits for the render thread */
        return t;
    }
    return 0;
}


/* ---- frames: screenshots for the QA scripts (COD_INPUT "shot=name") ------- */

static char shot_name[256];
static int shot_pending;

void render_shot_request(const char *name)
{
    snprintf(shot_name, sizeof(shot_name), "%s", name);
    shot_pending = 1;
}

void render_diag_set(int mode) { (void)mode; }
void render_shutdown(void) {}

#if defined(__vita__)
/* The Vita build is shot from the Vita3K window by tools/qa_vita3k.sh, which
 * watches for this file. */
static void save_shot(void)
{
    FILE *f = fopen("ux0:data/cod1/shot_now.txt", "w");
    if (f) {
        fputs(shot_name, f);
        fclose(f);
    }
    rt_log("screenshot %s requested", shot_name);
}
#else
/* The back buffer as a 24-bit BMP (bottom-up rows, as GL reads them). */
static void save_shot(void)
{
    int w, h;
    platform_get_drawable_size(&w, &h);
    int stride = (w * 3 + 3) & ~3;
    uint8_t *px = malloc((size_t)stride * h);
    if (!px)
        return;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px);
    char path[300];
    snprintf(path, sizeof(path), "%s.bmp", shot_name);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t size = 54u + (uint32_t)(stride * h);
        uint8_t hd[54] = { 'B', 'M' };
        memcpy(hd + 2, &size, 4);
        hd[10] = 54;
        hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        memcpy(hd + 22, &h, 4);
        hd[26] = 1;
        hd[28] = 24;
        fwrite(hd, 1, 54, f);
        fwrite(px, 1, (size_t)stride * h, f);
        fclose(f);
        rt_log("screenshot %s (%dx%d)", path, w, h);
    }
    free(px);
}
#endif

int g_gl_tracing;
extern int g_trace_next_frame;

void gl_trace_call(Ctx *c, const char *name)
{
    static uint32_t vptr, cptr, vsize = 3, vstride, cstride;
    rt_log("gl %s(%08X %08X %08X %08X %08X %08X)", name, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5));
    if (!strcmp(name, "glVertexPointer")) {
        vsize = ARG(0);
        vstride = ARG(2) ? ARG(2) : vsize * 4u;
        vptr = ARG(3);
    } else if (!strcmp(name, "glColorPointer")) {
        cstride = ARG(2) ? ARG(2) : 4u;
        cptr = ARG(3);
    } else if (!strcmp(name, "glDrawElements") && vptr) {
        /* the first vertices drawn (unsigned int indices) */
        uint32_t n = ARG(1) < 4u ? ARG(1) : 4u, idx = ARG(3);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t i = ARG(2) == 0x1405u ? RD32(idx + 4u * k) : ARG(2) == 0x1403u ? RD16(idx + 2u * k) : RD8(idx + k);
            float x, y, z;
            uint32_t a = vptr + i * vstride;
            memcpy(&x, gptr(a), 4);
            memcpy(&y, gptr(a + 4u), 4);
            memcpy(&z, gptr(a + 8u), 4);
            rt_log("    v%u: %.1f %.1f %.1f  rgba %08X", i, x, y, z, cptr ? RD32(cptr + i * cstride) : 0u);
        }
    }
}

static void draw_stats_frame(void);

/* Frame times while playing, logged every 10 seconds: the average, the slowest
 * frame and how many frames missed 30 fps or hitched. With the profiler,
 * COD_PROF_FRAMES picks the frames whose samples count: "play" (the default:
 * gameplay frames), "slow" (gameplay frames over 50 ms: the hitches) or "all". */
static void frame_times(void)
{
    static uint64_t last, window, freq, play_us;
    static uint32_t frames, over33, over50, over100, worst;
    static int prof_mode = -1;
    uint64_t now = SDL_GetPerformanceCounter();
    if (!freq) {
        freq = SDL_GetPerformanceFrequency();
        last = window = now;
#ifdef COD_PROFILE
        const char *m = getenv("COD_PROF_FRAMES");
        prof_mode = !m || !strcmp(m, "play") ? 1 : !strcmp(m, "slow") ? 2 : 0;
        if (prof_mode)
            rt_prof_reset(1);
#endif
        return;
    }
    uint32_t us = (uint32_t)((now - last) * 1000000u / freq);
    last = now;
    /* Played: no menu or console (keyCatchers 0, also true during videos), cls.state
     * active (2; 3 is a full-screen video) and not a loading screen's long frame. */
    int play = pad_in_gameplay() && RD32(0x01439260u) == 2u && us < 1000000u;
#ifdef COD_PROFILE
    if (prof_mode > 0)
        rt_prof_frame_end(play && (prof_mode == 1 || us > 50000u));
#endif
    (void)prof_mode;
    if (play) {
        frames++;
        play_us += us;
        over33 += us > 34000u;
        over50 += us > 50000u;
        over100 += us > 100000u;
        if (us > worst)
            worst = us;
    }
    uint64_t span = (now - window) * 1000u / freq;
    if (span >= 10000u) {
        if (frames)
            rt_log("frames: %u in play, %.1f fps, slowest %.0f ms, over 33 ms %u, over 50 ms %u, over 100 ms %u",
                   frames, frames * 1e6 / (double)play_us, worst / 1000.0, over33, over50, over100);
        window = now;
        frames = over33 = over50 = over100 = worst = 0;
        play_us = 0;
    }
}

static void present(void)
{
    frame_times();
    draw_stats_frame();
    vbo_frame++;
    if (g_gl_tracing) {
        g_gl_tracing = 0;
        g_trace_next_frame = 0;
        rt_log("gl trace: end of frame");
    } else if (g_trace_next_frame) {
        g_gl_tracing = 1;
        rt_log("gl trace: next frame");
    }
#if defined(__SWITCH__)
    gamma_apply();   /* the brightness setting, as the PC's display ramp would */
#endif
    if (shot_pending) {
        shot_pending = 0;
        save_shot();
    }
    platform_swap();
}

/* ---- gdi32 pixel formats, buffers and gamma ------------------------------ */

SHIM(gdi32, ChoosePixelFormat) { stdcall_ret(c, 1, 2); }
SHIM(gdi32, SetPixelFormat) { stdcall_ret(c, 1, 3); }

SHIM(gdi32, DescribePixelFormat)
{
    fill_pfd(ARG(3), ARG(2));
    stdcall_ret(c, 1, 4);   /* the number of formats */
}

SHIM(gdi32, SwapBuffers)
{
    present();
    stdcall_ret(c, 1, 1);
}

/* Gamma ramps: 3 x 256 WORDs. The game's brightness setting (r_gamma) goes
 * through them. The Switch applies them to every frame (gamma_switch.c); elsewhere
 * they are not applied yet. GetDeviceGammaRamp reports the identity: the display's
 * own ramp, which the game restores when it exits. */
SHIM(gdi32, GetDeviceGammaRamp)
{
#if defined(__SWITCH__)
    rt_log("gamma: GetDeviceGammaRamp (the game checks for a display ramp)");
#endif
    uint32_t p = ARG(1);
    for (uint32_t k = 0; k < 3u; k++)
        for (uint32_t i = 0; i < 256u; i++)
            WR16(p + (k * 256u + i) * 2u, i * 257u);
    stdcall_ret(c, 1, 2);
}

SHIM(gdi32, SetDeviceGammaRamp)
{
#if defined(__SWITCH__)
    uint16_t ramp[768];
    for (uint32_t i = 0; i < 768u; i++)
        ramp[i] = RD16(ARG(1) + 2u * i);
    static unsigned calls;
    if (calls++ < 40u)
        rt_log("gamma: SetDeviceGammaRamp #%u: red[32]=%04X red[64]=%04X red[128]=%04X red[255]=%04X", calls,
               ramp[32], ramp[64], ramp[128], ramp[255]);
    gamma_ramp_set(ramp);
#endif
    stdcall_ret(c, 1, 2);
}
