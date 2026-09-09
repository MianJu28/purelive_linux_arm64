#include "my_application.h"

#include <stdlib.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(__linux__)

// JM9100 (MWV207) VA-API driver discovery.
//
// libva does not resolve the `jmgpu` driver through its standard name map and
// fails with `vaGetDriverNames() failed` unless LIBVA_DRIVER_NAME is set
// (audit doc: docs/LINUX_JM9100_HWDECODE_AUDIT.md, problem P4). The desktop
// session exports it via /etc/profile.d/mwv207_glvnd.sh, but sessions not
// sourced from a login shell cannot rely on that, so inject it here before
// media_kit loads libmpv.
//
// The injection is deliberately minimal: only this single variable. Never add
// the mwv207 library directory to LD_LIBRARY_PATH from here - its generic
// names (libgbm.so.1, libOpenCL.so...) shadow system libraries and the setenv
// at runtime would not take effect for the dynamic loader anyway.
static void jm9100_inject_vaapi_driver_env(void) {
  if (getenv("LIBVA_DRIVER_NAME") != nullptr) {
    return;  // Respect an explicit user/distro setting.
  }

  if (access("/dev/jmgpu", F_OK) != 0) {
    return;  // No JM9100 device: not our hardware, keep the environment clean.
  }

  if (access("/usr/lib/aarch64-linux-gnu/dri/jmgpu_drv_video.so", F_OK) != 0) {
    return;  // VA-API driver package (com.jingjiamicro.mwv207.vaapi) missing.
  }

  setenv("LIBVA_DRIVER_NAME", "jmgpu", 0);
}

// JM9100 (MWV207) Flutter UI rendering environment.
//
// Once the Jingjia EGL vendor ICD works (audit doc §5), glvnd selects the
// mwv207 EGL library in this process and Flutter's Impeller OpenGLES backend
// dies with SIGSEGV inside the Jingjia GLES stack at startup. Forcing the
// Mesa vendor instead leaves the window black: Mesa cannot probe a driver
// for PCI 0731:9100 under X11 ("failed to create dri2 screen"), and Impeller
// cannot determine the GL version on the llvmpipe fallback. The stable
// combination on this hardware is therefore:
//   - UI: Skia (Impeller disabled in my_application.cc) on the Mesa software
//     path (llvmpipe), which is how the app ran before the EGL ICD fix.
//   - Playback: JMDEC hardware decoding kept through LIBVA_DRIVER_NAME=jmgpu
//     (injected above); the decoder does not depend on the UI EGL choice.
// Both variables respect explicit user settings and are only injected when
// the JM9100 device is present. Remove this injection once the Jingjia GLES
// stack passes Flutter's renderer, or switch it back to evaluate hardware UI
// compositing (audit doc §9, follow-up items).
static void jm9100_force_mesa_egl_for_ui(void) {
  if (getenv("LIBGL_ALWAYS_SOFTWARE") != nullptr) {
    return;  // Respect an explicit user/distro setting.
  }

  if (access("/dev/jmgpu", F_OK) != 0) {
    return;  // No JM9100 device: not our hardware, keep the environment clean.
  }

  if (access("/usr/share/glvnd/egl_vendor.d/50_mesa.json", F_OK) != 0) {
    return;  // Mesa vendor config missing: keep the environment untouched.
  }

  setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
         "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 0);
  setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
  // The Skia shell obtains its GL context through GDK/GLX, not the embedder's
  // own EGL display, so the GLX vendor must go back to mesa as well: the
  // mwv207 GLX ICD advertises no matching RGBA visual ("Failed to create
  // OpenGL context") and leaves the proc resolver without a GL_VERSION. This
  // overrides on purpose: /etc/profile.d/mwv207_glvnd.sh exports mwv207 for
  // the whole desktop session and would otherwise win, while the mwv207 GLX
  // ICD breaks every GLX client on this machine, not just this app.
  setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1);
}

#endif  // defined(__linux__)

int main(int argc, char** argv) {
#if defined(__linux__)
  jm9100_inject_vaapi_driver_env();
  jm9100_force_mesa_egl_for_ui();
#endif
  g_autoptr(MyApplication) app = my_application_new();
  return g_application_run(G_APPLICATION(app), argc, argv);
}
