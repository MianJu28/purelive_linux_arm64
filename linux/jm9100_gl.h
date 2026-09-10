#ifndef PURE_LIVE_JM9100_GL_H_
#define PURE_LIVE_JM9100_GL_H_

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

// JM9100 (MWV207) GL backend selection for the Linux runner.
//
// Background (docs/LINUX_JM9100_HWDECODE_AUDIT.md):
//
// * media_kit_video renders libmpv in an isolated EGL context derived from the
//   GDK display and shares every frame with Flutter through an EGLImage on that
//   same display, so the UI and the video decoder must run on the same GL
//   vendor. Forcing Mesa/llvmpipe for the UI (the previous workaround) therefore
//   also forces libmpv onto llvmpipe, where mpv cannot create the VA-API dmabuf
//   interop and silently degrades to `vaapi-copy` (no CPU win).
// * The hardware stack was unusable because the GTK/Flutter window uses the
//   screen's default visual, which the Jingjia GLX vendor does not expose an
//   FBConfig for, so `gdk_window_create_gl_context` failed with
//   "The RGBA pixel format has no available settings" and the window stayed
//   black. The vendor does expose other visuals (0x21 in the reference
//   environment), and a window created with one of those gets a real
//   "Jingjia JM9100" GL context.
//
// The runner therefore probes the session GL stack in a short-lived child
// process and can pick:
//
// * hardware: keep the session GL environment and give the window a visual the
//   active GLX vendor supports, so the engine gets a real "Jingjia JM9100" GL
//   context instead of a black window;
// * software: the safe Mesa/llvmpipe configuration (the default).
//
// The GPU stack stays opt-in (PURELIVE_JM9100_GL=hardware, or `auto` for the
// probe result) because the in-app video pipeline on it is not verified yet:
// with the GTK embedder's desktop GL context the bundled libmpv rejects the
// VA-API dmabuf interop, and the cross-API EGLImage sharing still needs a
// hardware sample.

// Selects the GL environment. Must run before the first GL call (the Flutter
// engine is created later, in my_application_activate).
void jm9100_gl_prepare_environment(const char* executable_path);

// Applies the probed visual to a not-yet-realized window. No-op in software
// mode or when no visual was selected.
void jm9100_gl_apply_window_visual(GtkWindow* window);

// True when this process was started as the probe child.
gboolean jm9100_gl_probe_requested(int argc, char** argv);

// Runs the probe and exits the process in the caller (0 = usable GPU stack).
int jm9100_gl_probe_main(void);

#ifdef __cplusplus
}
#endif

#endif  // PURE_LIVE_JM9100_GL_H_
