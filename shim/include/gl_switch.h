/* OpenGL on the Nintendo Switch (switch-mesa).
 *
 * Mesa there is reached through EGL only: there is no libGL exporting the gl*
 * entry points, so every function has to be looked up (eglGetProcAddress, via
 * SDL_GL_GetProcAddress). The generated table (opengl32_gen.c) already does that;
 * this header covers the few functions the hand-written code calls by name, by
 * turning each call into a call through a pointer that gl_switch_load() fills. */
#ifndef COD_GL_SWITCH_H
#define COD_GL_SWITCH_H

#include <GL/gl.h>
#include <GL/glext.h>
#include <stddef.h>

#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

/* name, return type, parameter list */
#define GL_SWITCH_FUNCS(X)                                                                                           \
    X(glBindBuffer, void, (GLenum target, GLuint buffer))                                                            \
    X(glBufferData, void, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))                          \
    X(glBufferSubData, void, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data))                    \
    X(glClientActiveTexture, void, (GLenum texture))                                                                 \
    X(glColorPointer, void, (GLint size, GLenum type, GLsizei stride, const void *pointer))                          \
    X(glDeleteBuffers, void, (GLsizei n, const GLuint *buffers))                                                      \
    X(glDrawRangeElements, void, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,                 \
                                  const void *indices))                                                              \
    X(glGenBuffers, void, (GLsizei n, GLuint *buffers))                                                              \
    X(glGetString, const GLubyte *, (GLenum name))                                                                   \
    X(glNormalPointer, void, (GLenum type, GLsizei stride, const void *pointer))                                     \
    X(glPixelStorei, void, (GLenum pname, GLint param))                                                              \
    X(glReadBuffer, void, (GLenum mode))                                                                             \
    X(glReadPixels, void, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,              \
                           void *pixels))                                                                            \
    X(glScissor, void, (GLint x, GLint y, GLsizei width, GLsizei height))                                            \
    X(glTexCoordPointer, void, (GLint size, GLenum type, GLsizei stride, const void *pointer))                       \
    X(glTexImage2D, void, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,          \
                           GLint border, GLenum format, GLenum type, const void *pixels))                            \
    X(glTexSubImage2D, void, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,              \
                              GLsizei height, GLenum format, GLenum type, const void *pixels))                       \
    X(glVertexPointer, void, (GLint size, GLenum type, GLsizei stride, const void *pointer))                         \
    X(glViewport, void, (GLint x, GLint y, GLsizei width, GLsizei height))

#define GL_SWITCH_DECLARE(name, ret, params) extern ret(APIENTRYP sw_##name) params;
GL_SWITCH_FUNCS(GL_SWITCH_DECLARE)
#undef GL_SWITCH_DECLARE

/* The calls by name go through the pointers (declared after <GL/gl.h>, whose
 * prototypes keep their names). */
#define glBindBuffer sw_glBindBuffer
#define glBufferData sw_glBufferData
#define glBufferSubData sw_glBufferSubData
#define glClientActiveTexture sw_glClientActiveTexture
#define glColorPointer sw_glColorPointer
#define glDeleteBuffers sw_glDeleteBuffers
#define glDrawRangeElements sw_glDrawRangeElements
#define glGenBuffers sw_glGenBuffers
#define glGetString sw_glGetString
#define glNormalPointer sw_glNormalPointer
#define glPixelStorei sw_glPixelStorei
#define glReadBuffer sw_glReadBuffer
#define glReadPixels sw_glReadPixels
#define glScissor sw_glScissor
#define glTexCoordPointer sw_glTexCoordPointer
#define glTexImage2D sw_glTexImage2D
#define glTexSubImage2D sw_glTexSubImage2D
#define glVertexPointer sw_glVertexPointer
#define glViewport sw_glViewport

/* Looks every pointer up; called on the GL thread once the context is current.
 * Returns the number of functions the driver did not have. */
int gl_switch_load(void *(*getproc)(const char *name));

#endif
