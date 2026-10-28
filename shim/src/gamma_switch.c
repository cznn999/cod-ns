/* The game's gamma ramp on the Switch.
 *
 * On the PC the brightness setting (r_gamma, with r_overBrightBits) goes to the
 * display through SetDeviceGammaRamp: the game draws its lightmaps darker than they
 * should look and the ramp brightens the whole picture on the way to the screen.
 * The Switch has no such ramp, so it is applied here: at the end of every frame the
 * back buffer is copied to a texture and drawn back through the ramp (a 256 x 1
 * lookup texture) by a small shader. While the ramp is the identity nothing is done.
 *
 * Runs on the thread that makes the GL calls (present() in opengl32.c); the ramp
 * itself is set by the game thread (SetDeviceGammaRamp). */
#if defined(__SWITCH__)
#include "gamma_switch.h"
#include "platform.h"
#include "runtime.h"

#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <string.h>

/* ---- the ramp (any thread) ------------------------------------------------- */

static pthread_mutex_t ramp_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t ramp_pending[3][256];
static unsigned ramp_gen;          /* bumped by every change */
static int ramp_identity = 1;

void gamma_ramp_set(const uint16_t ramp[768])
{
    int identity = 1;
    for (unsigned k = 0; k < 3u && identity; k++)
        for (unsigned i = 0; i < 256u; i++)
            if ((ramp[k * 256u + i] >> 8) != i) {
                identity = 0;
                break;
            }
    pthread_mutex_lock(&ramp_lock);
    memcpy(ramp_pending, ramp, sizeof(ramp_pending));
    ramp_identity = identity;
    ramp_gen++;
    pthread_mutex_unlock(&ramp_lock);
}

/* ---- GL (the GL thread) ------------------------------------------------------ */

typedef void(APIENTRY *pfn_bind_texture)(GLenum, GLuint);
typedef void(APIENTRY *pfn_tex_parameteri)(GLenum, GLenum, GLint);
typedef void(APIENTRY *pfn_tex_image_2d)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void(APIENTRY *pfn_tex_sub_image_2d)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void(APIENTRY *pfn_copy_tex_sub_image_2d)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
typedef void(APIENTRY *pfn_active_texture)(GLenum);
typedef void(APIENTRY *pfn_enum)(GLenum);
typedef void(APIENTRY *pfn_bitfield)(GLbitfield);
typedef void(APIENTRY *pfn_void)(void);
typedef void(APIENTRY *pfn_viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void(APIENTRY *pfn_color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void(APIENTRY *pfn_polygon_mode)(GLenum, GLenum);
typedef void(APIENTRY *pfn_2f)(GLfloat, GLfloat);
typedef void(APIENTRY *pfn_pixel_storei)(GLenum, GLint);
typedef void(APIENTRY *pfn_get_integerv)(GLenum, GLint *);
typedef GLuint(APIENTRY *pfn_create_shader)(GLenum);
typedef void(APIENTRY *pfn_shader_source)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void(APIENTRY *pfn_uint)(GLuint);
typedef GLuint(APIENTRY *pfn_create_program)(void);
typedef void(APIENTRY *pfn_attach_shader)(GLuint, GLuint);
typedef void(APIENTRY *pfn_get_iv)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *pfn_get_info_log)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLint(APIENTRY *pfn_get_uniform_location)(GLuint, const GLchar *);
typedef void(APIENTRY *pfn_uniform1i)(GLint, GLint);

static struct {
    pfn_bind_texture BindTexture;
    pfn_tex_parameteri TexParameteri;
    pfn_tex_image_2d TexImage2D;
    pfn_tex_sub_image_2d TexSubImage2D;
    pfn_copy_tex_sub_image_2d CopyTexSubImage2D;
    pfn_active_texture ActiveTexture;
    pfn_enum Disable, MatrixMode, Begin, ReadBuffer;
    pfn_bitfield PushAttrib, PushClientAttrib;
    pfn_void PopAttrib, PopClientAttrib, End;
    pfn_viewport Viewport;
    pfn_color_mask ColorMask;
    pfn_polygon_mode PolygonMode;
    pfn_2f TexCoord2f, Vertex2f;
    pfn_pixel_storei PixelStorei;
    pfn_get_integerv GetIntegerv;
    pfn_create_shader CreateShader;
    pfn_shader_source ShaderSource;
    pfn_uint CompileShader, LinkProgram, UseProgram;
    pfn_create_program CreateProgram;
    pfn_attach_shader AttachShader;
    pfn_get_iv GetShaderiv, GetProgramiv;
    pfn_get_info_log GetShaderInfoLog, GetProgramInfoLog;
    pfn_get_uniform_location GetUniformLocation;
    pfn_uniform1i Uniform1i;
} gl;

static int state;                  /* 0 not set up, 1 ready, -1 failed (pass off) */
static GLuint program, scene_tex, ramp_tex;
static int scene_w, scene_h;
static unsigned ramp_applied = ~0u;

static void *proc(const char *name, int *missing)
{
    void *p = platform_gl_proc(name);
    if (!p) {
        rt_log("gamma: %s not found", name);
        (*missing)++;
    }
    return p;
}

static const char *const vertex_src =
    "void main() {\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "    gl_Position = gl_Vertex;\n"
    "}\n";

/* Each channel through its own row of the ramp: texel i holds R[i], G[i], B[i]. */
static const char *const fragment_src =
    "uniform sampler2D scene;\n"
    "uniform sampler2D ramp;\n"
    "vec2 at(float v) { return vec2(v * (255.0 / 256.0) + 0.5 / 256.0, 0.5); }\n"
    "void main() {\n"
    "    vec3 c = texture2D(scene, gl_TexCoord[0].xy).rgb;\n"
    "    gl_FragColor = vec4(texture2D(ramp, at(c.r)).r, texture2D(ramp, at(c.g)).g,\n"
    "                        texture2D(ramp, at(c.b)).b, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char msg[512];
        gl.GetShaderInfoLog(s, sizeof(msg), NULL, msg);
        rt_log("gamma: shader not compiled: %s", msg);
        return 0;
    }
    return s;
}

static int setup(void)
{
    int missing = 0;
#define LOAD(field, name) *(void **)&gl.field = proc(name, &missing)
    LOAD(BindTexture, "glBindTexture");
    LOAD(TexParameteri, "glTexParameteri");
    LOAD(TexImage2D, "glTexImage2D");
    LOAD(TexSubImage2D, "glTexSubImage2D");
    LOAD(CopyTexSubImage2D, "glCopyTexSubImage2D");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(Disable, "glDisable");
    LOAD(MatrixMode, "glMatrixMode");
    LOAD(Begin, "glBegin");
    LOAD(ReadBuffer, "glReadBuffer");
    LOAD(PushAttrib, "glPushAttrib");
    LOAD(PushClientAttrib, "glPushClientAttrib");
    LOAD(PopAttrib, "glPopAttrib");
    LOAD(PopClientAttrib, "glPopClientAttrib");
    LOAD(End, "glEnd");
    LOAD(Viewport, "glViewport");
    LOAD(ColorMask, "glColorMask");
    LOAD(PolygonMode, "glPolygonMode");
    LOAD(TexCoord2f, "glTexCoord2f");
    LOAD(Vertex2f, "glVertex2f");
    LOAD(PixelStorei, "glPixelStorei");
    LOAD(GetIntegerv, "glGetIntegerv");
    LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource");
    LOAD(CompileShader, "glCompileShader");
    LOAD(LinkProgram, "glLinkProgram");
    LOAD(UseProgram, "glUseProgram");
    LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader");
    LOAD(GetShaderiv, "glGetShaderiv");
    LOAD(GetProgramiv, "glGetProgramiv");
    LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD(GetUniformLocation, "glGetUniformLocation");
    LOAD(Uniform1i, "glUniform1i");
#undef LOAD
    if (missing)
        return 0;
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_src), fs = compile(GL_FRAGMENT_SHADER, fragment_src);
    if (!vs || !fs)
        return 0;
    program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char msg[512];
        gl.GetProgramInfoLog(program, sizeof(msg), NULL, msg);
        rt_log("gamma: program not linked: %s", msg);
        return 0;
    }
    GLint prev_program = 0;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    gl.UseProgram(program);
    gl.Uniform1i(gl.GetUniformLocation(program, "scene"), 0);
    gl.Uniform1i(gl.GetUniformLocation(program, "ramp"), 1);
    gl.UseProgram((GLuint)prev_program);

    gl.PushAttrib(GL_TEXTURE_BIT);
    /* Fixed names far above the game's: the renderer picks its texture names itself
     * (it binds numbers of its own instead of asking glGenTextures), so a name GL
     * handed out here could later be bound and overwritten by the game, and the
     * game's own texture replaced by the frame copy. */
    scene_tex = 0x7F000001u;
    ramp_tex = 0x7F000002u;
    gl.BindTexture(GL_TEXTURE_2D, ramp_tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    gl.BindTexture(GL_TEXTURE_2D, scene_tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.PopAttrib();
    rt_log("gamma: ramp pass ready");
    return 1;
}

void gamma_apply(void)
{
    if (state < 0)
        return;
    uint8_t texels[256 * 3];
    unsigned gen;
    int identity;
    pthread_mutex_lock(&ramp_lock);
    gen = ramp_gen;
    identity = ramp_identity;
    if (gen != ramp_applied && !identity)
        for (unsigned i = 0; i < 256u; i++)
            for (unsigned k = 0; k < 3u; k++)
                texels[i * 3u + k] = (uint8_t)(ramp_pending[k][i] >> 8);
    pthread_mutex_unlock(&ramp_lock);
    static int logged_identity = -1;
    if (identity != logged_identity) {
        logged_identity = identity;
        rt_log("gamma: frames %s", identity ? "drawn as they are (identity ramp)" : "drawn through the ramp");
    }
    if (identity)
        return;
    if (!state) {
        state = setup() ? 1 : -1;
        if (state < 0) {
            rt_log("gamma: ramp pass unavailable, brightness setting has no effect");
            return;
        }
    }
    int w, h;
    platform_get_drawable_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;

    GLint prev_program = 0;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    gl.PushAttrib(GL_ALL_ATTRIB_BITS);
    gl.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, ramp_tex);
    if (gen != ramp_applied) {
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, texels);
        ramp_applied = gen;
    }
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, scene_tex);
    if (w != scene_w || h != scene_h) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        scene_w = w;
        scene_h = h;
    }
    gl.ReadBuffer(GL_BACK);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_ALPHA_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_FOG);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.Viewport(0, 0, w, h);
    gl.UseProgram(program);
    gl.Begin(GL_TRIANGLE_STRIP);
    gl.TexCoord2f(0.0f, 0.0f);
    gl.Vertex2f(-1.0f, -1.0f);
    gl.TexCoord2f(1.0f, 0.0f);
    gl.Vertex2f(1.0f, -1.0f);
    gl.TexCoord2f(0.0f, 1.0f);
    gl.Vertex2f(-1.0f, 1.0f);
    gl.TexCoord2f(1.0f, 1.0f);
    gl.Vertex2f(1.0f, 1.0f);
    gl.End();

    gl.UseProgram((GLuint)prev_program);
    gl.PopClientAttrib();
    gl.PopAttrib();
}

#else
typedef int gamma_switch_translation_unit_is_not_empty;
#endif
