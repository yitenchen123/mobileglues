// MobileGlues - egl/sdl_swap_gate.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "sdl_swap_gate.h"
#include "context.h"
#include "../gl/log.h"
#include "../includes.h"

#include <atomic>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <pthread.h>

// See sdl_swap_gate.h for the full account of what is being repaired and why the
// repair has to go through SDL rather than around it.
//
// The upstream fix this is ported from (MobileGLES-Wrapper fd632f2) was written
// for Android and hardcodes libSDL3.so. Two changes were needed here:
//
//   1. The library is located by walking a candidate list, so the same code
//      works on iOS (libSDL3.dylib), Android (libSDL3.so) and desktop. A
//      hardcoded name would silently no-op on every platform but one -- and
//      "silently no-op" is exactly the failure mode this code exists to remove.
//
//   2. The bound-context facts come from dev's egl/context.cpp (g_current_ctx,
//      MGContext) instead of Wrapper's AppRenderTarget. dev tracks the current
//      context per thread with a pointer only eglMakeCurrent writes, which is
//      the same information and is already maintained for gl/multidraw.cpp.
//
// Attempt accounting is deliberately conservative: only the thread that owns the
// binding may repair, because SDL's gate is thread local and writing another
// thread's TLS would achieve nothing. The attempt budget is small and stops
// permanently once SDL's own accounting shows the gate open.

namespace {
    std::atomic<bool> g_presented{false};
    std::atomic<int> g_attempts{0};
    std::atomic<bool> g_gate_probe_logged{false};
    thread_local bool t_in_repair = false;

    // Bounded so a pathological host cannot spin here: the repair is a startup
    // correction, not a per-frame one.
    constexpr int kMaxAttempts = 8;
}

// ---------------------------------------------------------------------------
// Locating SDL
// ---------------------------------------------------------------------------

// dlopen with RTLD_NOLOAD: locate a library that is ALREADY loaded, never load
// one. Loading SDL from inside the GL backend would be both wrong (this layer
// must not change what the host links) and dangerous (a real dlopen on the
// render thread can contend with dyld's lock).
static void* OpenSdlIfLoaded() {
    static void* cached = nullptr;
    static bool probed = false;
    if (probed) return cached;
    probed = true;

    // Ordered so the platform's own naming comes first; the rest are cheap
    // misses that make the code portable without a preprocessor maze.
    static const char* candidates[] = {
#if defined(__APPLE__)
        "libSDL3.dylib",
        "libSDL3.0.dylib",
#else
        "libSDL3.so",
        "libSDL3.so.0",
#endif
        "SDL3",
    };

    for (const char* name : candidates) {
        cached = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
        if (cached != nullptr) {
            LOG_D("SDL gate: located %s for the swap-gate repair", name)
            return cached;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The repair
// ---------------------------------------------------------------------------

static void RepairSdlCurrentWindow() {
    // Re-entry guard. SDL_GL_MakeCurrent calls back into this library's
    // eglMakeCurrent, so without this the repair could call itself.
    if (t_in_repair) return;

    MGContext* ctx = g_current_ctx;
    if (ctx == nullptr || ctx->handle == EGL_NO_CONTEXT || ctx->draw == EGL_NO_SURFACE) return;

    void* sdl = OpenSdlIfLoaded();
    if (sdl == nullptr) {
        // SDL not loaded yet. Not an error, and deliberately not latched: a
        // later attempt will see it.
        return;
    }

    auto get_window = reinterpret_cast<void* (*)()>(dlsym(sdl, "SDL_GL_GetCurrentWindow"));
    auto make_current = reinterpret_cast<bool (*)(void*, void*)>(dlsym(sdl, "SDL_GL_MakeCurrent"));
    auto get_windows = reinterpret_cast<void** (*)(int*)>(dlsym(sdl, "SDL_GetWindows"));
    if (get_window == nullptr || make_current == nullptr || get_windows == nullptr) {
        LOG_W_FORCE("SDL gate: SDL is loaded but SDL_GL_GetCurrentWindow/SDL_GL_MakeCurrent/SDL_GetWindows could "
                    "not all be resolved (got %p, %p, %p); the swap-gate repair cannot run",
                    reinterpret_cast<void*>(get_window), reinterpret_cast<void*>(make_current),
                    reinterpret_cast<void*>(get_windows))
        return;
    }

    void* current = get_window();
    if (current != nullptr) {
        // The gate is open. Nothing to repair, and nothing to report.
        g_presented.store(true, std::memory_order_relaxed);
        return;
    }

    int count = 0;
    void** windows = get_windows(&count);
    if (windows == nullptr || count <= 0) {
        LOG_W_FORCE("SDL gate: SDL_GL_GetCurrentWindow() is NULL and SDL_GetWindows reported %d window(s); "
                    "nothing to bind, so the gate cannot be reopened",
                    count)
        return;
    }

    t_in_repair = true;
    const bool ok = make_current(windows[0], reinterpret_cast<void*>(ctx->handle));
    t_in_repair = false;

    LOG_W_FORCE("SDL gate: SDL_GL_GetCurrentWindow() was NULL -- SDL cleared it on release and its EGL layer "
                "ignores eglMakeCurrent's return value, so refusing the release cannot prevent it. Re-stated the "
                "bind through SDL_GL_MakeCurrent(window=%p, ctx=%p) -> %d; current window is now %p%s",
                windows[0], reinterpret_cast<void*>(ctx->handle), static_cast<int>(ok), get_window(),
                ok ? " (gate open)" : " (still closed)")
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void mg_sdl_gate_note_presented() {
    // Reaching eglSwapBuffers at all means SDL admitted us. Record it and stop.
    if (!g_presented.exchange(true, std::memory_order_relaxed)) {
        LOG_D("SDL gate: first presentation reached the backend; the gate is open and no repair is needed")
    }
    g_attempts.store(kMaxAttempts, std::memory_order_relaxed);
}

void mg_sdl_gate_maybe_repair() {
    if (g_presented.load(std::memory_order_relaxed)) return;
    if (g_attempts.load(std::memory_order_relaxed) >= kMaxAttempts) return;

    // Only the thread that owns the binding: SDL's gate lives in thread-local
    // storage, so a repair from any other thread would write a TLS nobody reads.
    MGContext* ctx = g_current_ctx;
    if (ctx == nullptr) return;

    g_attempts.fetch_add(1, std::memory_order_relaxed);
    RepairSdlCurrentWindow();
}

// Funnel for the per-frame call sites. The two atomic reads below are the whole
// cost on a healthy run: g_retired is true from the first presented frame onward,
// and on a host without SDL g_attempts saturates after the first few calls. No
// dlopen, no dlsym, no lock -- those only happen on the handful of attempts
// before the answer is known.
//
// A per-frame trigger is necessary and not merely convenient. The failure being
// repaired can occur with no eglMakeCurrent to latch onto: SDL clears its
// thread-local on release, the launcher then hands the game back the same
// window, and the game never binds again. In that sequence the only evidence is
// that draws keep happening while swaps never arrive -- so the draw path is the
// one place guaranteed to be visited while the gate is shut.
void mg_sdl_gate_tick() {
    if (g_presented.load(std::memory_order_relaxed)) {
        // Presenting normally. Retire permanently rather than testing an atomic
        // per draw for the rest of the session; a later closure cannot happen
        // without a release, and a release is always followed by a bind.
        g_attempts.store(kMaxAttempts, std::memory_order_relaxed);
        return;
    }
    if (g_attempts.load(std::memory_order_relaxed) >= kMaxAttempts) return;
    mg_sdl_gate_maybe_repair();
}

void mg_sdl_gate_probe(const char* when) {
    if (g_gate_probe_logged.exchange(true)) return;

    void* sdl = OpenSdlIfLoaded();
    if (sdl == nullptr) {
        LOG_I("SDL gate probe (%s): SDL is not loaded -- this host does not present through SDL, so the "
              "swap-gate repair is inactive",
              when ? when : "?")
        return;
    }

    auto get_window = reinterpret_cast<void* (*)()>(dlsym(sdl, "SDL_GL_GetCurrentWindow"));
    auto get_windows = reinterpret_cast<void** (*)(int*)>(dlsym(sdl, "SDL_GetWindows"));

    void* current = get_window ? get_window() : nullptr;
    int count = 0;
    void** windows = get_windows ? get_windows(&count) : nullptr;

    MGContext* ctx = g_current_ctx;
    LOG_I("SDL gate probe (%s): loaded=1 current_window=%p windows=%d first_window=%p ctx=%p draw_surface=%p%s",
          when ? when : "?", current, count, (windows && count > 0) ? windows[0] : nullptr,
          ctx ? reinterpret_cast<void*>(ctx->handle) : nullptr,
          ctx ? reinterpret_cast<void*>(ctx->draw) : nullptr,
          current == nullptr ? " -- gate CLOSED, repair will run on the next GL call" : " -- gate open")
}
