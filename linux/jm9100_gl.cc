#include "jm9100_gl.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdk/gdk.h>
#if defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#endif

namespace {

constexpr char kProbeSwitch[] = "--jm9100-gl-probe";
constexpr char kModeVariable[] = "PURELIVE_JM9100_GL";
constexpr char kCompatVariable[] = "JMGPU_GL_COMPAT";
constexpr int kProbeTimeoutSeconds = 15;

// Result of the last environment decision.
gboolean g_hardware_selected = FALSE;
unsigned long g_visual_id = 0;

bool devicePresent() { return access("/dev/jmgpu", F_OK) == 0; }

// --- GL compat for the Jingjia stack -----------------------------------------
//
// The vendor's user space declares GL_OES_EGL_image and implements
// glEGLImageTargetTexture2DOES, but has no glEGLImageTargetTexStorageEXT at all:
// that entry point is a glvnd-generated no-op stub, so a client that takes the
// GL_EXT_EGL_image_storage route imports a dmabuf into a texture that stays all
// zero (jm9100 README §3.6/§3.8). The system-wide fix there is an LD_PRELOAD
// shim; an application can do the same in-process:
//
//  * media_kit_video's get_proc_address wrapper advertises
//    GL_EXT_EGL_image_storage to libmpv (video_output.cc), and
//  * the interposer below redirects the storage entry point, which libmpv
//    resolves through libEGL directly, onto the working OES entry point.
//
// Together they are equivalent to mpv's mpv_dmabuf_oes_image.patch and let the
// pinned libmpv archive stay untouched (a locally rebuilt, dynamically linked
// libmpv breaks the Flutter embedder's surface creation on this driver).
bool compatEnabled() {
  const char* disabled = g_getenv(kCompatVariable);
  if (disabled != nullptr && strcmp(disabled, "0") == 0) {
    return FALSE;
  }
  return devicePresent();
}

typedef void (*EglProc)(void);

void* libEglHandle() {
  static void* handle = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  return handle;
}

EglProc realEglGetProcAddressImpl(const char* name) {
  static auto real = reinterpret_cast<EglProc (*)(const char*)>([] {
    void* handle = libEglHandle();
    return handle != nullptr ? dlsym(handle, "eglGetProcAddress") : nullptr;
  }());
  return real != nullptr ? real(name) : nullptr;
}

// Only the Jingjia renderer needs the redirection: Mesa implements the storage
// entry properly and must keep using it, otherwise the software fallback would
// silently switch to a different (immutable texture) life cycle.
bool jingjiaRendererInUse() {
  static int cached = -1;
  if (cached >= 0) {
    return cached == 1;
  }
  auto current_context = reinterpret_cast<EGLContext (*)(void)>([] {
    void* handle = libEglHandle();
    return handle != nullptr ? dlsym(handle, "eglGetCurrentContext") : nullptr;
  }());
  if (current_context == nullptr || current_context() == EGL_NO_CONTEXT) {
    return FALSE;  // No EGL context here; keep the vendor answer.
  }
  auto get_string = reinterpret_cast<const GLubyte* (*)(GLenum)>(
      realEglGetProcAddressImpl("glGetString"));
  if (get_string == nullptr) {
    return FALSE;
  }
  const char* renderer = reinterpret_cast<const char*>(get_string(GL_RENDERER));
  if (renderer == nullptr) {
    return FALSE;
  }
  cached = strstr(renderer, "Jingjia") != nullptr ? 1 : 0;
  return cached == 1;
}

}  // namespace

// libepoxy maps the EGL entry points onto its own wrappers, so the macro has to
// be lifted before this file can provide the real symbol that interposes.
#if defined(eglGetProcAddress)
#undef eglGetProcAddress
#endif

// Interposes the process-wide libEGL entry point lookup so libmpv (loaded later
// by media_kit) resolves the storage entry point to the OES one that actually
// binds the imported dmabuf.
extern "C" EglProc eglGetProcAddress(const char* procname) {
  if (procname == nullptr) {
    return nullptr;
  }
  if (compatEnabled() &&
      (strcmp(procname, "glEGLImageTargetTexStorageEXT") == 0 ||
       strcmp(procname, "glEGLImageTargetTextureStorageEXT") == 0) &&
      jingjiaRendererInUse()) {
    EglProc oes = realEglGetProcAddressImpl("glEGLImageTargetTexture2DOES");
    if (oes != nullptr) {
      static gboolean logged = FALSE;
      if (!logged) {
        logged = TRUE;
        g_print("jm9100: redirecting %s to glEGLImageTargetTexture2DOES\n", procname);
      }
      return oes;
    }
  }
  return realEglGetProcAddressImpl(procname);
}

namespace {

#if defined(GDK_WINDOWING_X11)
unsigned long visualIdOf(GdkVisual* visual) {
  Visual* xvisual = gdk_x11_visual_get_xvisual(visual);
  return xvisual != nullptr ? static_cast<unsigned long>(xvisual->visualid) : 0UL;
}
#endif

// Tries to build the GL context the Flutter/GTK embedder needs on one visual.
// The GTK shell renders through the GDK/GLX context, so a visual is usable only
// when the active GL vendor exposes an FBConfig for it; the screen default
// visual (0x21 here) and GTK's own fallback (0x100) have no vendor config and
// leave the window black.
gboolean tryVisual(GdkVisual* visual, unsigned long* visual_id, gchar** renderer) {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  if (window == nullptr) {
    return FALSE;
  }
  gtk_window_set_default_size(GTK_WINDOW(window), 64, 64);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_widget_set_visual(window, visual);
  gtk_widget_show_all(window);
  while (gtk_events_pending()) {
    gtk_main_iteration();
  }

  gboolean ok = FALSE;
  GdkWindow* gdk_window = gtk_widget_get_window(window);
  if (gdk_window != nullptr) {
    GError* error = nullptr;
    GdkGLContext* context = gdk_window_create_gl_context(gdk_window, &error);
    if (context == nullptr) {
      g_printerr("jm9100: probe visual 0x%lx (depth %d): context failed (%s)\n",
                 visualIdOf(visual), gdk_visual_get_depth(visual),
                 error != nullptr ? error->message : "unknown");
      g_clear_error(&error);
    } else {
      gdk_gl_context_set_use_es(context, TRUE);
      gdk_gl_context_set_required_version(context, 2, 0);
      if (!gdk_gl_context_realize(context, &error)) {
        g_printerr("jm9100: probe visual 0x%lx (depth %d): realize failed (%s)\n",
                   visualIdOf(visual), gdk_visual_get_depth(visual),
                   error != nullptr ? error->message : "unknown");
        g_clear_error(&error);
      } else {
        gdk_gl_context_make_current(context);
        ok = TRUE;
        *visual_id = visualIdOf(visual);
        if (renderer != nullptr) {
          const char* value = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
          *renderer = g_strdup(value != nullptr ? value : "unknown");
        }
      }
    }
  }

  gtk_widget_destroy(window);
  while (gtk_events_pending()) {
    gtk_main_iteration();
  }
  return ok;
}

// Runs in the probe child: reports which visual of the current session gives a
// GL context the embedder can render with.
int runProbeChild() {
  // g_spawn_sync has no timeout; a wedged driver must not hang application
  // startup, so the child terminates itself.
  alarm(kProbeTimeoutSeconds);

  int argc = 1;
  char* argv[] = {const_cast<char*>("pure_live"), nullptr};
  char** argv_pointer = argv;
  if (!gtk_init_check(&argc, &argv_pointer)) {
    return 4;
  }

  GdkScreen* screen = gdk_screen_get_default();
  if (screen == nullptr) {
    return 4;
  }

  GdkVisual* system_visual = gdk_screen_get_system_visual(screen);
  GList* visuals = gdk_screen_list_visuals(screen);

  unsigned long visual_id = 0;
  gchar* renderer = nullptr;
  gboolean found = FALSE;

  // Candidate order, measured on the JM9100: the vendor only exposes a GLX
  // FBConfig for its own 24-bit visual (0x21/0x22) and for the 32-bit ARGB
  // visuals, while GTK's own fallback (0x100) has none. A window on 0x21 is
  // created successfully but then fails during presentation (GLXBadPixmap), so
  // the 32-bit ARGB visuals - the ones a composited GTK window prefers - are
  // tried first and the remaining ones stay as a last resort.
  for (int pass = 0; pass < 3 && !found; ++pass) {
    for (GList* item = visuals; item != nullptr && !found; item = item->next) {
      GdkVisual* visual = GDK_VISUAL(item->data);
      if (visual == nullptr) {
        continue;
      }
      const gboolean is_system = visual == system_visual;
      const gboolean is_argb = gdk_visual_get_depth(visual) == 32;
      const gboolean wanted = pass == 0   ? (is_argb && !is_system)
                              : pass == 1 ? (is_argb && is_system)
                                          : (!is_argb && is_system);
      if (!wanted) {
        continue;
      }
      found = tryVisual(visual, &visual_id, &renderer);
    }
  }

  if (visuals != nullptr) {
    g_list_free(visuals);
  }

  if (!found) {
    return 4;
  }

  g_print("PURELIVE_JM9100_PROBE=ok\n");
  g_print("PURELIVE_JM9100_VISUAL=0x%lx\n", visual_id);
  g_print("PURELIVE_JM9100_RENDERER=%s\n", renderer != nullptr ? renderer : "unknown");
  g_free(renderer);
  return 0;
}

// Spawns this executable in probe mode and parses its report.
gboolean probeSessionGl(const char* executable_path, unsigned long* visual_id, gchar** renderer) {
  gchar* executable = g_file_read_link("/proc/self/exe", nullptr);
  if (executable == nullptr || executable[0] == '\0') {
    g_free(executable);
    if (executable_path == nullptr || executable_path[0] == '\0') {
      return FALSE;
    }
    executable = g_strdup(executable_path);
  }

  gchar* argv[] = {executable, const_cast<gchar*>(kProbeSwitch), nullptr};
  gchar* standard_output = nullptr;
  gint wait_status = 0;
  GError* error = nullptr;
  gboolean spawned = g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                                  &standard_output, nullptr, &wait_status, &error);
  if (!spawned) {
    g_printerr("jm9100: GL probe could not start (%s); keeping the safe software stack\n",
               error != nullptr ? error->message : "unknown");
    g_clear_error(&error);
    g_free(executable);
    g_free(standard_output);
    return FALSE;
  }
  g_free(executable);

  gboolean ok = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
  if (!ok) {
    g_printerr("jm9100: GL probe found no usable GPU GL context "
               "(status=%d); keeping the safe software stack\n",
               wait_status);
    g_free(standard_output);
    return FALSE;
  }

  unsigned long parsed_visual = 0;
  gchar* parsed_renderer = nullptr;
  if (standard_output != nullptr) {
    gchar** lines = g_strsplit(standard_output, "\n", -1);
    for (int i = 0; lines[i] != nullptr; ++i) {
      unsigned long value = 0;
      if (sscanf(lines[i], "PURELIVE_JM9100_VISUAL=0x%lx", &value) == 1) {
        parsed_visual = value;
      }
      if (g_str_has_prefix(lines[i], "PURELIVE_JM9100_RENDERER=")) {
        g_free(parsed_renderer);
        parsed_renderer = g_strdup(lines[i] + strlen("PURELIVE_JM9100_RENDERER="));
      }
    }
    g_strfreev(lines);
  }
  g_free(standard_output);

  if (parsed_visual == 0) {
    g_printerr("jm9100: GL probe succeeded without reporting a visual\n");
    g_free(parsed_renderer);
    return FALSE;
  }

  *visual_id = parsed_visual;
  if (renderer != nullptr) {
    *renderer = parsed_renderer;
  } else {
    g_free(parsed_renderer);
  }
  return TRUE;
}

// The JM9100 stack could not serve the embedder; keep the long-standing
// Mesa/llvmpipe configuration: Mesa EGL for the engine and libmpv, the Mesa GLX
// vendor for GDK, and Mesa's software rasterizer because the X server cannot
// create a DRI2/DRI3 screen for PCI 0731:9100.
void applySoftwareGlEnvironment() {
  if (access("/usr/share/glvnd/egl_vendor.d/50_mesa.json", F_OK) != 0) {
    return;
  }
  setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 0);
  setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
  // Overrides on purpose: the desktop session exports mwv207 for every GLX
  // client (/etc/profile.d/mwv207_glvnd.sh), and that ICD fails the visual
  // lookup the GTK embedder performs.
  setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1);
}

}  // namespace

gboolean jm9100_gl_probe_requested(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && strcmp(argv[i], kProbeSwitch) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

int jm9100_gl_probe_main(void) { return runProbeChild(); }

// Presentation sync for the JM9100 X11 session.
//
// The X server has no hardware GLX for PCI 0731:9100 (the vendor DDX misses the
// current ABI, so the server falls back to DRISWRAST), and no compositor owns
// _NET_WM_CM_S0 in this session. GLX swaps therefore reach the framebuffer
// without any vblank relationship, and the scanout shows a partially written
// buffer: bands and triangular patches of the previous frame inside the live
// picture (docs/LINUX_JM9100_HWDECODE_AUDIT.md §10.9). Mesa-derived GLX clients
// honour the traditional __GL_SYNC_TO_VBLANK switch, which restores an even
// presentation. The plugin's video render thread uses EGL; enabling
// vblank_mode=1 there was measured to slow that thread down and make frames
// repeat/rewind, so it is deliberately left off.
void applyPresentationSyncEnvironment() {
  // PURELIVE_JM9100_VBLANK: 0 = no sync vars; 1 = Mesa weak sync (vblank_mode=1);
  // unset or 3 = strongest (vblank_mode=3). Measured on this box: without any
  // sync the presentation shows band/triangle patches of the previous frame
  // (30 fps sources severe, 60 fps occasional, §10.9); vblank_mode=1 reduces
  // them; 3 is the strongest Mesa sync level.
  const gchar* mode = g_getenv("PURELIVE_JM9100_VBLANK");
  if (mode != nullptr && g_ascii_strcasecmp(mode, "0") == 0) {
    g_print("jm9100: presentation vblank sync disabled (PURELIVE_JM9100_VBLANK=0)\n");
    return;
  }
  // Overwrite=0 keeps values the user set explicitly.
  setenv("__GL_SYNC_TO_VBLANK", "1", 0);
  const char* vblank_mode =
      (mode != nullptr && g_ascii_strcasecmp(mode, "1") == 0) ? "1" : "3";
  setenv("vblank_mode", vblank_mode, 0);
  g_print("jm9100: presentation sync __GL_SYNC_TO_VBLANK=1 vblank_mode=%s\n", vblank_mode);
}

void jm9100_gl_prepare_environment(const char* executable_path) {
  if (!devicePresent()) {
    return;  // Not JM9100 hardware: keep the environment untouched.
  }

  applyPresentationSyncEnvironment();

  const gchar* requested = g_getenv(kModeVariable);
  if (requested != nullptr && g_ascii_strcasecmp(requested, "software") == 0) {
    g_print("jm9100: GL backend forced to software (%s=software)\n", kModeVariable);
    applySoftwareGlEnvironment();
    return;
  }

  unsigned long visual_id = 0;
  gchar* renderer = nullptr;
  const gboolean probed = probeSessionGl(executable_path, &visual_id, &renderer);

  if (!probed) {
    const gboolean forced_hardware =
        requested != nullptr && g_ascii_strcasecmp(requested, "hardware") == 0;
    if (forced_hardware) {
      g_printerr("jm9100: the GPU GL stack was requested but the probe failed; "
                 "the window keeps the default visual\n");
      g_hardware_selected = TRUE;
      return;
    }
    g_print("jm9100: no usable GPU GL context for the Flutter embedder; "
            "using Mesa software rendering\n");
    applySoftwareGlEnvironment();
    return;
  }

  g_hardware_selected = TRUE;
  g_visual_id = visual_id;
  g_print("jm9100: GPU GL stack enabled (visual 0x%lx, renderer %s)\n", visual_id,
          renderer != nullptr ? renderer : "unknown");
  g_free(renderer);
}

void jm9100_gl_apply_window_visual(GtkWindow* window) {
  if (!g_hardware_selected || g_visual_id == 0 || window == nullptr) {
    return;
  }
#if defined(GDK_WINDOWING_X11)
  GdkScreen* screen = gtk_window_get_screen(window);
  if (screen == nullptr || !GDK_IS_X11_SCREEN(screen)) {
    return;
  }

  // The window must use the visual the probe validated: GTK's own choice has no
  // vendor FBConfig on this driver and renders nothing.
  const gchar* override = g_getenv("PURELIVE_JM9100_VISUAL");
  unsigned long requested = g_visual_id;
  if (override != nullptr && override[0] != '\0') {
    requested = strtoul(override, nullptr, 0);
  }

  GdkVisual* visual = gdk_x11_screen_lookup_visual(GDK_X11_SCREEN(screen), requested);
  if (visual == nullptr) {
    g_printerr("jm9100: X visual 0x%lx is unavailable; keeping the default visual\n", requested);
    return;
  }
  gtk_widget_set_visual(GTK_WIDGET(window), visual);
  g_print("jm9100: window visual set to 0x%lx\n", requested);
#endif
}
