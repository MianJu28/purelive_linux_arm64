#include "jm9100_gl.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <epoxy/gl.h>
#include <gdk/gdk.h>
#if defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#endif

namespace {

constexpr char kProbeSwitch[] = "--jm9100-gl-probe";
constexpr char kModeVariable[] = "PURELIVE_JM9100_GL";
constexpr int kProbeTimeoutSeconds = 15;

// Result of the last environment decision.
gboolean g_hardware_selected = FALSE;
unsigned long g_visual_id = 0;

bool devicePresent() { return access("/dev/jmgpu", F_OK) == 0; }

unsigned long visualIdOf(GdkVisual* visual) {
#if defined(GDK_WINDOWING_X11)
  Visual* xvisual = gdk_x11_visual_get_xvisual(visual);
  if (xvisual != nullptr) {
    return static_cast<unsigned long>(xvisual->visualid);
  }
#endif
  return 0;
}

// Tries to build the same GL context the Flutter embedder needs (EGL/GLES2
// through the window's visual) on one candidate visual.
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
      g_printerr("jm9100: probe visual 0x%lx: context failed (%s)\n", visualIdOf(visual),
                 error != nullptr ? error->message : "unknown");
      g_clear_error(&error);
    } else {
      gdk_gl_context_set_use_es(context, TRUE);
      gdk_gl_context_set_required_version(context, 2, 0);
      if (!gdk_gl_context_realize(context, &error)) {
        g_printerr("jm9100: probe visual 0x%lx: realize failed (%s)\n", visualIdOf(visual),
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
// working GL context for the embedder path.
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

  // The system visual is what GTK picks on its own, so it is tried first; every
  // other visual of the screen is the fallback set the vendor may support.
  for (int pass = 0; pass < 2 && !found; ++pass) {
    for (GList* item = visuals; item != nullptr && !found; item = item->next) {
      GdkVisual* visual = GDK_VISUAL(item->data);
      if (visual == nullptr) {
        continue;
      }
      if ((pass == 0) != (visual == system_visual)) {
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
// vendor for GDK (the mwv207 GLX ICD advertises no matching visual), and Mesa's
// software rasterizer because the X server cannot create a DRI2/DRI3 screen for
// PCI 0731:9100.
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

void jm9100_gl_prepare_environment(const char* executable_path) {
  if (!devicePresent()) {
    return;  // Not JM9100 hardware: keep the environment untouched.
  }

  const gchar* requested = g_getenv(kModeVariable);
  const gboolean hardware_requested =
      requested != nullptr && (g_ascii_strcasecmp(requested, "hardware") == 0 ||
                               g_ascii_strcasecmp(requested, "auto") == 0);

  if (!hardware_requested) {
    // Default remains the long-standing Mesa/llvmpipe configuration. The GPU GL
    // stack is opt-in until the in-app video pipeline on it is verified end to
    // end: the bundled libmpv refuses the VA-API dmabuf interop for the desktop
    // GL context the GTK embedder ends up with (see the audit document), and the
    // cross-API EGLImage sharing has not been sampled on hardware yet.
    g_print("jm9100: using Mesa software rendering (set %s=hardware for the GPU GL stack)\n",
            kModeVariable);
    applySoftwareGlEnvironment();
    return;
  }

  unsigned long visual_id = 0;
  gchar* renderer = nullptr;
  const gboolean probed = probeSessionGl(executable_path, &visual_id, &renderer);

  if (!probed) {
    // Explicit request: keep the session GL environment anyway rather than
    // silently falling back, so the failure stays visible to the caller.
    g_printerr("jm9100: the GPU GL stack was requested but no usable visual was found; "
               "the window keeps the default visual\n");
    g_hardware_selected = TRUE;
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
  GdkVisual* visual = gdk_x11_screen_lookup_visual(GDK_X11_SCREEN(screen), g_visual_id);
  if (visual == nullptr) {
    g_printerr("jm9100: X visual 0x%lx is unavailable; keeping the default visual\n",
               g_visual_id);
    return;
  }
  // The embedder resolves its GL config from the window's visual, so this must
  // happen before the window is realized.
  gtk_widget_set_visual(GTK_WIDGET(window), visual);
  g_print("jm9100: window visual set to 0x%lx\n", g_visual_id);
#endif
}
