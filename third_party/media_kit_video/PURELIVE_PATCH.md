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

## Bundled libmpv patch (JM9100 VA-API dmabuf)

`linux/CMakeLists.txt` downloads the media-kit libmpv archive into `lib/`. On the
Jingjia JM9100 the embedder ends up with a *desktop* GL context, and mpv then
requires `GL_EXT_EGL_image_storage` for the VA-API dmabuf interop. That driver
resolves `glEGLImageTargetTexStorageEXT` but never binds the imported surface
(only its `GL_OES_EGL_image` entry works) and omits the storage extension string,
so the upstream archive refuses the interop (`VAAPI hwdec only works with OpenGL
or Vulkan backends`) and the app falls back to `vaapi-copy`/software decoding.

`patches/mpv-0.41-vaapi-dmabuf-oes.patch` fixes that: the OES route is accepted
as an alternative to the storage route, and the OES entry is used whenever the
stack does not advertise the storage extension. Stacks that do advertise it keep
their previous behavior.

Build the archive from upstream mpv `v0.41.0` (the version media-kit's pinned
archive is based on) plus the two media-kit patches from
`Predidit/libmpv-linux-build@patch/mpv` (`mpv-fix-gles-sampler-precision.patch`,
`mpv-fix-libmpv-fd-leak.patch`):

```sh
git clone --depth 1 --branch v0.41.0 https://github.com/mpv-player/mpv.git
cd mpv
git apply /path/to/mpv-fix-gles-sampler-precision.patch
git apply /path/to/mpv-fix-libmpv-fd-leak.patch
git apply /path/to/purelive/third_party/media_kit_video/patches/mpv-0.41-vaapi-dmabuf-oes.patch
meson setup build -Dlibmpv=true -Dcplayer=false -Dvulkan=disabled -Dgpl=true -Dbuildtype=release
ninja -C build
mkdir -p pkg && cp build/libmpv.so.2.5.0 pkg/libmpv.so.2
(cd pkg && zip -9 libmpv_aarch64.zip libmpv.so.2)
```

Point the build at that archive (CMake cache variable or environment variable);
without it `flutter build linux` silently restores the unpatched download:

```sh
PURELIVE_LIBMPV_ZIP=/path/to/libmpv_aarch64.zip flutter build linux --release
```

The archive must contain `libmpv.so.2` with SONAME `libmpv.so.2`. On this
machine the resulting library is linked against the system FFmpeg/libplacebo
(6.1.5/6.338.2) instead of media-kit's static build; its `mpv_*` export set is
identical to the pinned archive, so media_kit's bindings stay satisfied.

Verification used for the patch (device-independent, no app UI needed): a libmpv
render-context probe that creates a GLX desktop-GL context first (like the GTK
embedder) and then the isolated EGL/GLES2 context, plays a solid-red H.264 clip
with `hwdec=vaapi` and reads the rendered FBO back. Unpatched archive:
`VAAPI hwdec only works with OpenGL or Vulkan backends`, `hwdec-current=no`.
Patched archive: `Using EGL dmabuf interop via GL_OES_EGL_image`,
`hwdec-current=vaapi`, frame pixels equal to the software-decoded ones (no
all-zero NV12 "green screen").

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
7. Re-apply `patches/mpv-0.41-vaapi-dmabuf-oes.patch` to the newly pinned
   libmpv source and rebuild the archive; when the pinned media-kit revision
   changes the mpv version, port the patch to that file revision first and
   re-run the libmpv probe described above.
