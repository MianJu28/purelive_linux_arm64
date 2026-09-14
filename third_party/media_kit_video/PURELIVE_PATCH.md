# PureLive media_kit_video patch

- Upstream: `https://github.com/Predidit/media-kit.git`
- Base commit: `994465d9bfca3f39d0b41199d16e7fd93fe97881`
- Package version: `media_kit_video 1.2.5`
- License: MIT; the upstream `LICENSE` is retained in this directory.

## Why this copy exists

On Android, `AndroidVideoController` owns the `vo`, `wid` and Surface lifecycle.
PureLive's room-scoped audio mode also needs to select `vid=no` without replacing
the player or reopening the live stream. Sending that property independently
could race a rotation, PiP or Surface resize update and leave the UI waiting for
a video track or Surface that had already vanished.

This patch adds `VideoController.setVideoOutputEnabled` and makes the Android
controller the single owner of both the requested video-output state and the
Surface lifecycle. Track properties are issued from the controller's lock via
media_kit's asynchronous mpv request. The synchronous string-property FFI call
is deliberately avoided for headphone switching because a busy live demuxer
can block Flutter's isolate before the audio presentation or timeout paints.
Video mode always selects `vid=auto`, including while WID is temporarily zero;
only an explicit audio-only request selects `vid=no`. This avoids a startup
deadlock where disabling video before a Surface callback also prevented the
callback that would restore it. Surface replacement follows Flutter's
`SurfaceProducer` contract: every availability/resize queries `getSurface()`, a
changed Java Surface receives a new JNI global reference, and the old WID is
detached exactly once before delayed reference deletion. Geometry-only updates
do not reset `vo`, and Surface changes do not seek a live stream merely to
refresh rendering. The Android Surface-size MethodChannel request remains
outside the controller lock. Desktop platforms retain media_kit's existing
`setVideoTrack` behavior.

On Windows, PureLive also exposes a throttled `frameRevision` liveness signal.
The native D3D11 mailbox emits it only after a fence-confirmed frame has been
promoted for Flutter consumption; software rendering emits it after a completed
render. This lets `PlayerManager` distinguish “libmpv still says playing” from
“the presentation surface has stopped advancing”, recreate the renderer once,
then fall through to the existing CDN-line recovery. The signal carries no
pixels, is limited to two events per second, and does not alter normal frame
delivery or aspect-ratio policy.

`VideoController.setSize` also accepts an opt-in `force` flag on every platform
(only the Windows native implementation changes behavior). PureLive uses it on
the first layout after a Windows `Texture` remount so the current native output
receives a viewport even when its controller cache still contains equal width
and height. Normal resize calls keep the upstream equality fast path. Together
with the frame-progress fence, this prevents a 0×0 replacement output from
being treated as presentation-ready after an overlay route or transport retry.

## Jingjia (JM9100) VA-API dmabuf compat

The pinned libmpv archive is deliberately used **unmodified**. On the Jingjia
JM9100 the embedder ends up with a desktop GL context, and mpv then requires
`GL_EXT_EGL_image_storage` for its VA-API dmabuf interop. That vendor declares
`GL_OES_EGL_image` and implements `glEGLImageTargetTexture2DOES`, but
`glEGLImageTargetTexStorageEXT` is only a glvnd no-op stub (jm9100 README
§3.6/§3.8), so the upstream archive refuses the interop
(`VAAPI hwdec only works with OpenGL or Vulkan backends`) and the app falls back
to `vaapi-copy`/software decoding.

Replacing the archive with a locally rebuilt libmpv was tried and rejected: an
mpv 0.41.0 build linked against the system FFmpeg/libplacebo makes the Flutter
embedder fail its surface creation on this driver (`Could not wrap embedder
supplied frame-buffer`, 88-133 errors, black window), while the pinned archive
renders normally with the same window visual.

The fix therefore lives inside the process and mirrors the system-wide
`jm_gl_compat.c` shim from the driver repository:

- `linux/video_output.cc` (`purelive_get_proc_address`): libmpv loads all GL
  entry points through this resolver, so it receives wrappers that advertise
  `GL_EXT_EGL_image_storage` (via `glGetString`, `glGetStringi` and
  `GL_NUM_EXTENSIONS`) and let mpv pick the storage route;
- `linux/jm9100_gl.cc` (`eglGetProcAddress` interposition, requires
  `ENABLE_EXPORTS` on the runner): the storage entry point, which libmpv resolves
  through libEGL directly, is redirected onto the working
  `glEGLImageTargetTexture2DOES`.

Both only engage when the current renderer is the Jingjia stack
(`JMGPU_GL_COMPAT=0` disables them), so other vendors keep upstream behavior.

Verification: a libmpv render-context probe that creates a GLX desktop-GL context
first (like the GTK embedder) and then the isolated EGL/GLES2 context, plays a
solid-red H.264 clip with `hwdec=vaapi` and reads the rendered FBO back. Without
the compat: `VAAPI hwdec only works with OpenGL or Vulkan backends`,
`hwdec-current=no`. With it: `Using EGL dmabuf interop via
GL_EXT_EGL_image_storage`, `hwdec-current=vaapi`, frame pixels equal to the
software-decoded ones (no all-zero NV12 "green screen").

## Maintenance

When updating the pinned media-kit revision:

1. Replace this directory with the new upstream package.
2. Reapply the controller API, Android state-owner patch, and Windows
   fence-confirmed frame-progress callback.
3. Compare every file against the new upstream commit; only the files described
   above, `pubspec.yaml`, this note and the policy helper should differ.
4. Run `flutter analyze`, the full test suite, Windows release build and Android
   ARM64 release build.
5. On Android, repeat video/audio toggles plus rotation, PiP and room re-entry.
6. On Windows, verify a deliberately stalled renderer is recreated once, a
   stalled CDN advances to the next line, a 0×0 candidate never replaces the
   active texture, remounting reasserts viewport size, and explicit pause never
   triggers the watchdog.
7. Keep the pinned libmpv archive untouched and re-check the Jingjia compat
   wrappers (`purelive_get_proc_address`, `eglGetProcAddress` interposition):
   they depend on media_kit still loading GL entry points through
   `MPV_RENDER_PARAM_OPENGL_INIT_PARAMS.get_proc_address` and on mpv still
   resolving the dmabuf interop entry points via `eglGetProcAddress`. Re-run the
   libmpv probe described above after any media-kit or mpv version change.
