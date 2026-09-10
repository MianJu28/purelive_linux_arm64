#include "my_application.h"

#include <stdlib.h>

#if defined(__linux__)
#include <unistd.h>

#include "jm9100_gl.h"
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

#endif  // defined(__linux__)

int main(int argc, char** argv) {
#if defined(__linux__)
  // The GL backend decision needs a GTK/GL probe, which must not run in this
  // process: loading a GL vendor early would freeze the choice for the whole
  // life of the process. The runner therefore re-executes itself in probe mode
  // (see linux/jm9100_gl.cc) and then picks the environment for the real UI.
  if (jm9100_gl_probe_requested(argc, argv)) {
    return jm9100_gl_probe_main();
  }

  jm9100_inject_vaapi_driver_env();

  // Keeps the safe Mesa/llvmpipe configuration by default and switches the
  // window (and the GL environment) to the GPU stack on request; see
  // linux/jm9100_gl.h for why the GPU stack is still opt-in.
  jm9100_gl_prepare_environment(argv[0]);
#endif
  g_autoptr(MyApplication) app = my_application_new();
  return g_application_run(G_APPLICATION(app), argc, argv);
}
