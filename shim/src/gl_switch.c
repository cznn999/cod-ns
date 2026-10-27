/* Pointers for the GL functions the hand-written code calls by name on the
 * Switch (see gl_switch.h). */
#if defined(__SWITCH__)
#include "gl_switch.h"
#include "runtime.h"

#define GL_SWITCH_DEFINE(name, ret, params) ret(APIENTRYP sw_##name) params;
GL_SWITCH_FUNCS(GL_SWITCH_DEFINE)
#undef GL_SWITCH_DEFINE

int gl_switch_load(void *(*getproc)(const char *name))
{
    int missing = 0;
#define GL_SWITCH_LOAD(name, ret, params)                                                                           \
    if (!sw_##name) {                                                                                                \
        *(void **)&sw_##name = getproc(#name);                                                                      \
        if (!sw_##name) {                                                                                            \
            /* Mesa has the ARB names of the buffer functions as aliases. */                                        \
            *(void **)&sw_##name = getproc(#name "ARB");                                                            \
            if (!sw_##name) {                                                                                        \
                rt_log("GL: %s not found", #name);                                                                  \
                missing++;                                                                                           \
            }                                                                                                        \
        }                                                                                                            \
    }
    GL_SWITCH_FUNCS(GL_SWITCH_LOAD)
#undef GL_SWITCH_LOAD
    return missing;
}

#endif
