// MobileGlues - egl/loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "loader.h"
#include "context.h"
#include "../gl/envvars.h"
#include "../gl/log.h"
#include "../gl/mg.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "trace.h"
#include <EGL/egl.h>
#include <string.h>

#define DEBUG 0

static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static EGLSurface eglSurface = EGL_NO_SURFACE;
static EGLContext eglContext = EGL_NO_CONTEXT;

// The bootstrap context in its role as a fallback for context-less threads.
// Separate from the three above, which belong to the probe and are cleared by
// destroy_temp_egl_ctx on the failure path.
static EGLDisplay g_fallback_display = EGL_NO_DISPLAY;
static EGLSurface g_fallback_surface = EGL_NO_SURFACE;
static EGLContext g_fallback_context = EGL_NO_CONTEXT;
static bool g_fallback_available = false;

void init_target_egl() {
    ETRACE("init_target_egl: starting the bootstrap probe")
    LOAD_EGL(eglGetProcAddress);
    LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglQueryString);
    LOAD_EGL(eglTerminate);
    LOAD_EGL(eglGetError);

    EGLint configAttribs[] = {EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              8,
                              EGL_SURFACE_TYPE,
                              EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES3_BIT,
                              EGL_NONE};

    EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

    EGLint pbAttribs[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};

    EGLConfig pbufConfig;
    EGLint configsFound = 0;

    eglDisplay = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    ETRACE("init_target_egl: eglGetDisplay(EGL_DEFAULT_DISPLAY) -> %p", eglDisplay)
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOG_E("eglGetDisplay failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (egl_eglInitialize(eglDisplay, NULL, NULL) == EGL_TRUE) {
        mg_display_initialised(eglDisplay, true);
    } else {
        LOG_E("eglInitialize failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (egl_eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        LOG_E("eglBindAPI failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (egl_eglChooseConfig(eglDisplay, configAttribs, &pbufConfig, 1, &configsFound) != EGL_TRUE) {
        LOG_E("eglChooseConfig failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (configsFound == 0) {
        // Index 7, the value, not index 6, which is EGL_ALPHA_SIZE itself. Writing
        // the name slot turned the pair into attribute 0 -- not an EGL attribute at
        // all -- so the retry always failed with EGL_BAD_ATTRIBUTE and the "no
        // alpha" fallback this is meant to be never actually ran.
        configAttribs[7] = 0;
        if (egl_eglChooseConfig(eglDisplay, configAttribs, &pbufConfig, 1, &configsFound) != EGL_TRUE) {
            LOG_E("Retry eglChooseConfig failed (0x%x)", egl_eglGetError());
            goto cleanup;
        }
        if (configsFound) {
            LOG_D("Using config without alpha channel");
        } else {
            LOG_E("No valid EGL config found");
            goto cleanup;
        }
    }

    eglContext = egl_eglCreateContext(eglDisplay, pbufConfig, EGL_NO_CONTEXT, ctxAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        LOG_E("eglCreateContext failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }
    ETRACE("init_target_egl: probe context %p", eglContext)

    eglSurface = egl_eglCreatePbufferSurface(eglDisplay, pbufConfig, pbAttribs);
    if (eglSurface == EGL_NO_SURFACE) {
        LOG_E("eglCreatePbufferSurface failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }
    ETRACE("init_target_egl: probe surface %p", eglSurface)

    if (egl_eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) != EGL_TRUE) {
        LOG_E("eglMakeCurrent failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    LOG_V("EGL initialized successfully");
    ETRACE("init_target_egl: probe up (dpy=%p, ctx=%p, surface=%p)", eglDisplay, eglContext, eglSurface)
    return;

cleanup:
    if (eglSurface != EGL_NO_SURFACE) {
        egl_eglDestroySurface(eglDisplay, eglSurface);
        eglSurface = EGL_NO_SURFACE;
    }
    if (eglContext != EGL_NO_CONTEXT) {
        egl_eglDestroyContext(eglDisplay, eglContext);
        eglContext = EGL_NO_CONTEXT;
    }
    // Through the holder count, exactly like the success path does. This used to
    // terminate unconditionally, so any step after eglInitialize failing -- a
    // config the driver would not give us, a pbuffer it would not make -- tore
    // down EGL_DEFAULT_DISPLAY underneath the host process, taking contexts and
    // surfaces it had created before this library was ever loaded.
    if (mg_display_release(eglDisplay, true)) {
        egl_eglTerminate(eglDisplay);
    }
    // Cleared so destroy_temp_egl_ctx, which runs at the end of proc_init whether
    // or not the probe came up, does not destroy the same handles a second time
    // and release a display this function has already let go of.
    eglDisplay = EGL_NO_DISPLAY;
    LOG_E("EGL initialization failed");
    ETRACE("init_target_egl: probe FAILED, all three handles released")
}

// The bootstrap context is kept, not destroyed.
//
// It used to be torn down at the end of proc_init, which left the process with a
// driver that answers nothing on any thread that has not since bound a context of
// its own. A query from such a thread does not fail loudly: glGetIntegerv leaves
// the caller's buffer alone (so a zeroed LWJGL buffer stays zero) and glGetString
// returns NULL. Minecraft 26.3 divides by one of those zeros and crashes, and
// calls strlen on the NULL and crashes, and both have been reported in the field.
//
// Keeping one small pbuffer context alive costs a 32x32 surface and gives every
// thread a way to ask the driver a question and get a true answer. It is bound
// only for the duration of a query that would otherwise have no context, and
// released only if this layer was the one that bound it -- see
// BindFallbackEGLContextIfNeeded.
//
// destroy_temp_egl_ctx remains for the failure path in init_target_egl, which
// still has to release the display when the probe did not come up.
void mg_keep_bootstrap_context() {
    ETRACE("mg_keep_bootstrap_context: keeping dpy=%p, ctx=%p, surface=%p for context-less query threads",
           eglDisplay, eglContext, eglSurface)
    g_fallback_display = eglDisplay;
    g_fallback_context = eglContext;
    g_fallback_surface = eglSurface;
    g_fallback_available = (eglDisplay != EGL_NO_DISPLAY && eglContext != EGL_NO_CONTEXT);
}

// Bind the bootstrap context, but only if this thread has none.
//
// Returns true when this call is the one that bound it, and the caller must then
// pass the same value to Unbind. A thread that already had a context is left
// alone: its own context describes the surface it is really rendering to, and
// swapping it out for a pbuffer mid-frame would be a far worse bug than the NULL
// this exists to avoid.
bool BindFallbackEGLContextIfNeeded() {
    if (!g_fallback_available) return false;
    // g_current_ctx is written only by eglMakeCurrent, so it is exactly the
    // question being asked: does this thread already have a context of its own?
    if (g_current_ctx != nullptr) return false;

    LOAD_EGL(eglMakeCurrent);
    if (egl_eglMakeCurrent == nullptr) return false;

    if (!egl_eglMakeCurrent(g_fallback_display, g_fallback_surface, g_fallback_surface, g_fallback_context)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_E("the bootstrap context could not be bound on a thread that has none, so a query from this thread "
                  "will reach a driver with no current context and may answer nothing")
        }
        return false;
    }
    return true;
}

void UnbindFallbackEGLContext() {
    if (!g_fallback_available) return;

    LOAD_EGL(eglMakeCurrent);
    if (egl_eglMakeCurrent == nullptr) return;

    // Releasing through eglMakeCurrent is what the host expects. The bootstrap
    // handles themselves stay alive for the next thread that asks; only this
    // thread's binding goes away.
    egl_eglMakeCurrent(g_fallback_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void destroy_temp_egl_ctx() {
    ETRACE("destroy_temp_egl_ctx: dpy=%p, ctx=%p, surface=%p", eglDisplay, eglContext, eglSurface)
    if (eglDisplay == EGL_NO_DISPLAY) return;

    // The handles may already have been promoted to the shared fallback set by
    // mg_keep_bootstrap_context. Destroying them here would then leave that set
    // naming dead handles, so drop the promotion first.
    if (g_fallback_display == eglDisplay) {
        g_fallback_display = EGL_NO_DISPLAY;
        g_fallback_context = EGL_NO_CONTEXT;
        g_fallback_surface = EGL_NO_SURFACE;
        g_fallback_available = false;
    }

    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);

    egl_eglMakeCurrent(eglDisplay, 0, 0, EGL_NO_CONTEXT);
    if (eglSurface != EGL_NO_SURFACE) egl_eglDestroySurface(eglDisplay, eglSurface);
    if (eglContext != EGL_NO_CONTEXT) egl_eglDestroyContext(eglDisplay, eglContext);
    eglSurface = EGL_NO_SURFACE;
    eglContext = EGL_NO_CONTEXT;

    // Terminate only if this probe was the sole holder. EGL does not
    // reference-count initialisation per caller, so terminating unconditionally
    // marked every resource on EGL_DEFAULT_DISPLAY for destruction, including
    // contexts and surfaces the host process created before this library was
    // loaded.
    if (mg_display_release(eglDisplay, true)) {
        LOAD_EGL(eglTerminate);
        if (egl_eglTerminate) egl_eglTerminate(eglDisplay);
    }
    eglDisplay = EGL_NO_DISPLAY;
}
