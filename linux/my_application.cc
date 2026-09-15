#include "my_application.h"

#include <flutter_linux/flutter_linux.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#endif

#include "flutter/generated_plugin_registrant.h"

#if defined(__linux__)
#include "jm9100_gl.h"
#endif

struct _MyApplication {
  GtkApplication parent_instance;
  char** dart_entrypoint_arguments;
};

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

// PureLive: make sure the freshly launched window is actually on screen.
//
// Under KWin (deepin) the window is sometimes mapped but never adopted by the
// window manager: it has no _NET_WM_DESKTOP and is absent from _NET_CLIENT_LIST,
// so the compositor never draws it and it never receives focus - the user sees
// "the app started but there is no window". This is reproduced reliably on the
// first launch after the previous instance was killed: the window stays
// WM-unmanaged for minutes (verified by polling _NET_WM_DESKTOP / the WM client
// list every 250 ms). The only reliable remedy is to withdraw and map the window
// again, which makes the WM adopt it as a brand new window.

// A window the WM manages has _NET_WM_DESKTOP set; an unmapped/ignored one does
// not. This is the precise signal we need, independent of focus-stealing policy.
static gboolean window_managed_by_wm(GtkWindow* window) {
#ifdef GDK_WINDOWING_X11
  GdkWindow* gdk_window = gtk_widget_get_window(GTK_WIDGET(window));
  if (gdk_window == nullptr || !GDK_IS_X11_WINDOW(gdk_window)) {
    return TRUE;
  }
  Display* display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdk_window));
  Window xid = gdk_x11_window_get_xid(gdk_window);
  Atom net_wm_desktop = XInternAtom(display, "_NET_WM_DESKTOP", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  int status = XGetWindowProperty(display, xid, net_wm_desktop, 0, 1, False,
                                  AnyPropertyType, &actual_type, &actual_format,
                                  &nitems, &bytes_after, &data);
  gboolean managed = (status == Success && actual_type != None);
  if (data != nullptr) {
    XFree(data);
  }
  return managed;
#else
  (void)window;
  return TRUE;
#endif
}

static gboolean clear_keep_above_cb(gpointer data) {
  gtk_window_set_keep_above(GTK_WINDOW(data), FALSE);
  return G_SOURCE_REMOVE;
}

static void present_and_raise(GtkWindow* window) {
  gtk_window_present(window);
  GdkWindow* gdk_window = gtk_widget_get_window(GTK_WIDGET(window));
  if (gdk_window != nullptr) {
    gdk_window_raise(gdk_window);
  }
}

typedef struct {
  GtkWindow* window;  // owned (g_object_ref) for the lifetime of the retry loop
  guint attempts;
} VisRetry;

static gboolean ensure_window_visible_cb(gpointer data);

static void schedule_retry(VisRetry* retry, guint ms) {
  g_timeout_add_full(G_PRIORITY_DEFAULT, ms, ensure_window_visible_cb, retry,
                     nullptr);
}

// Runs a few times after startup. Stops as soon as the window is both managed by
// the WM and focused. If the WM never adopted the window, withdraw+map it so the
// WM picks it up as a new window. As a last resort, hold it above the others for
// a moment so the user at least sees it.
static gboolean ensure_window_visible_cb(gpointer data) {
  VisRetry* retry = static_cast<VisRetry*>(data);
  GtkWindow* window = retry->window;

  present_and_raise(window);

  if (window_managed_by_wm(window) && gtk_window_is_active(window)) {
    g_object_unref(window);
    g_free(retry);
    return G_SOURCE_REMOVE;
  }

  retry->attempts += 1;
  if (retry->attempts <= 5) {
    if (!window_managed_by_wm(window)) {
      // The WM never adopted the window (mapped but unmanaged: not composited,
      // never focused). Withdrawing and mapping it again makes the WM treat it
      // as a fresh window and adopt it.
      gtk_widget_hide(GTK_WIDGET(window));
      gtk_widget_show(GTK_WIDGET(window));
    }
    schedule_retry(retry, 500);
    return G_SOURCE_REMOVE;
  }

  if (!window_managed_by_wm(window)) {
    gtk_widget_hide(GTK_WIDGET(window));
    gtk_widget_show(GTK_WIDGET(window));
  }
  gtk_window_set_keep_above(window, TRUE);
  g_timeout_add_full(G_PRIORITY_DEFAULT, 4000, clear_keep_above_cb,
                     g_object_ref(window), g_object_unref);
  g_object_unref(window);
  g_free(retry);
  return G_SOURCE_REMOVE;
}

// Implements GApplication::activate.
static void my_application_activate(GApplication* application) {
  MyApplication* self = MY_APPLICATION(application);
  GtkWindow* window =
      GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));

  // Use a header bar when running in GNOME as this is the common style used
  // by applications and is the setup most users will be using (e.g. Ubuntu
  // desktop).
  // If running on X and not using GNOME then just use a traditional title bar
  // in case the window manager does more exotic layout, e.g. tiling.
  // If running on Wayland assume the header bar will work (may need changing
  // if future cases occur).
  gboolean use_header_bar = TRUE;
#ifdef GDK_WINDOWING_X11
  GdkScreen* screen = gtk_window_get_screen(window);
  if (GDK_IS_X11_SCREEN(screen)) {
    const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen);
    if (g_strcmp0(wm_name, "GNOME Shell") != 0) {
      use_header_bar = FALSE;
    }
  }
#endif
  if (use_header_bar) {
    GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
    gtk_widget_show(GTK_WIDGET(header_bar));
    gtk_header_bar_set_title(header_bar, "pure_live");
    gtk_header_bar_set_show_close_button(header_bar, TRUE);
    gtk_window_set_titlebar(window, GTK_WIDGET(header_bar));
  } else {
    gtk_window_set_title(window, "pure_live");
  }

  gtk_window_set_default_size(window, 1280, 720);

#if defined(__linux__)
  // The JM9100 GL selection (linux/jm9100_gl.cc) may hand the window a visual
  // that the active GLX vendor actually exposes an FBConfig for. Without it the
  // GTK/Flutter GL context creation fails on the Jingjia stack and the window
  // stays black. Must run before the window is realized.
  jm9100_gl_apply_window_visual(window);
#endif

  gtk_widget_show(GTK_WIDGET(window));

  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_dart_entrypoint_arguments(project, self->dart_entrypoint_arguments);
  // Flutter 3.47 ships the Impeller OpenGLES backend on Linux. On the JM9100
  // (mwv207) stack it dies with SIGSEGV inside the Jingjia GLES library, and
  // on the llvmpipe fallback it cannot determine the GL version, which leaves
  // the window black (docs/LINUX_JM9100_HWDECODE_AUDIT.md §9). Skia is the
  // long-established Linux rendering path, so keep it for desktop builds.
  fl_dart_project_set_enable_impeller(project, FALSE);

  FlView* view = fl_view_new(project);
  gtk_widget_show(GTK_WIDGET(view));
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

  fl_register_plugins(FL_PLUGIN_REGISTRY(view));

  gtk_widget_grab_focus(GTK_WIDGET(view));

  // PureLive: the upstream Flutter Linux template only shows the window, so a
  // window manager may keep a freshly launched window behind the active one, or
  // (under KWin) never adopt it at all - both look like "the app started but
  // there is no window". Re-check shortly after mapping and, if the WM refused
  // to manage/activate the window, withdraw+map it so the WM adopts it. The
  // callback removes itself once the window is managed and focused.
  gtk_window_set_urgency_hint(GTK_WINDOW(window), TRUE);
  VisRetry* retry = g_new0(VisRetry, 1);
  retry->window = GTK_WINDOW(g_object_ref(window));
  retry->attempts = 0;
  schedule_retry(retry, 800);
}

// Implements GApplication::local_command_line.
static gboolean my_application_local_command_line(GApplication* application, gchar*** arguments, int* exit_status) {
  MyApplication* self = MY_APPLICATION(application);
  // Strip out the first argument as it is the binary name.
  self->dart_entrypoint_arguments = g_strdupv(*arguments + 1);

  g_autoptr(GError) error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
     g_warning("Failed to register: %s", error->message);
     *exit_status = 1;
     return TRUE;
  }

  g_application_activate(application);
  *exit_status = 0;

  return TRUE;
}

// Implements GObject::dispose.
static void my_application_dispose(GObject* object) {
  MyApplication* self = MY_APPLICATION(object);
  g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev);
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
}

static void my_application_class_init(MyApplicationClass* klass) {
  G_APPLICATION_CLASS(klass)->activate = my_application_activate;
  G_APPLICATION_CLASS(klass)->local_command_line = my_application_local_command_line;
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
}

static void my_application_init(MyApplication* self) {}

MyApplication* my_application_new() {
  return MY_APPLICATION(g_object_new(my_application_get_type(),
                                     "application-id", APPLICATION_ID,
                                     "flags", G_APPLICATION_NON_UNIQUE,
                                     nullptr));
}
