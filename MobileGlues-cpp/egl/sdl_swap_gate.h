// MobileGlues - egl/sdl_swap_gate.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

// ---------------------------------------------------------------------------
// SDL's own swap gate, and the one thing about it this library has to repair.
//
// Ported from MobileGLES-Wrapper's egl/loader.cpp (commit fd632f2,
// "fix: 黑屏根因定位并修复 —— SDL 的换页门控被它自己清空，且无从阻止").
//
// The problem it solves, stated in SDL's own terms:
//
//   SDL_GL_SwapWindow refuses to present unless the window argument matches a
//   value SDL keeps in thread-local storage (src/video/SDL_video.c):
//
//       if (SDL_GL_GetCurrentWindow() != window) return SDL_SetError(...);
//
//   and that value is written only when a bind is reported successful:
//
//       result = _this->GL_MakeCurrent(_this, window, context);
//       if (result) { _this->current_glwin = window;
//                     SDL_SetTLS(&_this->current_glwin_tls, window, NULL); }
//
//   On the release path SDL reaches SDL_EGL_MakeCurrent, which DISCARDS the
//   return value of eglMakeCurrent and returns true unconditionally:
//
//       if (!egl_context || ...) {
//           eglMakeCurrent(display, NO_SURFACE, NO_SURFACE, NO_CONTEXT);
//       } else { ... }
//       return true;
//
//   so the release is reported as success and current_glwin becomes NULL.
//
// Nothing re-binds afterwards when the launcher reuses the primary window: the
// game is handed the same window back and sees no reason to call MakeCurrent
// again. From then on every swap is refused inside SDL and none of them ever
// reaches this library's eglSwapBuffers. The signature in a log is hundreds of
// thousands of GL calls and not one eglSwapBuffers -- the game runs, audio
// plays, and the screen stays black.
//
// Returning EGL_FALSE from this library's eglMakeCurrent cannot prevent it: the
// value is discarded before anyone reads it. Two attempts at that approach were
// made and both were inert.
//
// What works is to restate the bind through SDL's own public API, so SDL writes
// its own thread-local. This library knows the context is in fact current on a
// real surface -- it bound it -- so re-stating it is a correction of SDL's
// bookkeeping, not a bypass of it. The swap still goes through SDL, still passes
// SDL's gate, and still arrives at this library's eglSwapBuffers.
//
// Two facts make the restatement possible:
//   - SDL_GLContext is the EGLContext pointer on the EGL backend:
//     SDL_EGL_CreateContext returns (SDL_GLContext)egl_context, and
//     SDL_EGL_MakeCurrent casts it straight back.
//   - SDL_GetWindows is exported, so the window can be enumerated.
// ---------------------------------------------------------------------------

#ifndef MOBILEGLUES_EGL_SDL_SWAP_GATE_H
#define MOBILEGLUES_EGL_SDL_SWAP_GATE_H

// Called after a successful eglSwapBuffers.
//
// An arrival here is itself the proof that SDL's gate admitted us, so a
// presentation is recorded and the repair stops trying. The check is made on
// this path rather than on every GL call because the failure being repaired is
// "swaps never arrive": if a swap arrives, the gate is open and there is
// nothing left to do.
void mg_sdl_gate_note_presented();

// Called from eglMakeCurrent after a successful bind, and periodically from the
// GL call path.
//
// Cheap on the overwhelmingly common path: it returns immediately once a
// presentation has been observed, and does nothing at all on hosts without SDL
// loaded.
void mg_sdl_gate_maybe_repair();

// Per-frame funnel for the GL call path, called from glDrawElements.
//
// Necessary because the failure can occur with no eglMakeCurrent to hang the
// repair on: SDL clears its thread-local on release, the launcher then hands the
// game back the same window, and the game never binds again. The only remaining
// evidence is that draws keep happening while swaps never arrive -- so a draw
// entry point is the one place guaranteed to be visited while the gate is shut.
//
// Two relaxed atomic loads and a return on a healthy run. No dlopen, no dlsym,
// no lock on that path; those happen only during the handful of attempts before
// the answer is known.
void mg_sdl_gate_tick();

// Diagnostics only: logs the three SDL facts the repair depends on (whether the
// library is loaded, what the current window is, how many windows exist) plus
// the gate state, once. Intended for a single call at startup so a log shows
// whether the premise holds on a given host.
void mg_sdl_gate_probe(const char* when);

#endif // MOBILEGLUES_EGL_SDL_SWAP_GATE_H
