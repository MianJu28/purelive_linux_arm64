# PureLive media_kit_video 说明

- 来源：本仓库上游 `master` 的 `third_party/media_kit_video`（`media_kit_video 1.2.5`，
  源自 `https://github.com/Predidit/media-kit.git`）。
- License: MIT；上游 `LICENSE` 随目录保留。
- 本目录相对上游**只有一处补丁**：Jingjia（JM9100）VA-API dmabuf 兼容层（见下节），
  落在 `linux/video_output.cc` 与 `linux/CMakeLists.txt`。

历史说明：PureLive 曾在 `994465d9` 基线上自行追加 `VideoController.setVideoOutputEnabled`、
Windows `frameRevision` 帧进度回调与 `setSize(force:)`。这些能力已由上游副本自带
（`lib/` 侧的调用方同样是上游实现），因此不再重复打补丁；本目录只在 Jinjia 兼容层上
与上游存在差异。

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

## 维护

上游更新 media-kit 时的步骤：

1. 用新的上游包替换本目录（保持目录名 `third_party/media_kit_video`）。
2. 重新应用 Jingjia 兼容补丁：`linux/video_output.cc` 的 `purelive_get_proc_address`
   包装与 `linux/CMakeLists.txt` 的说明注释，可用
   `git diff upstream/master <上一版本> -- third_party/media_kit_video/linux` 取回。
3. 与本目录对比，除上述文件与本说明外不应有其它差异；`pubspec.yaml` 的差异只允许是
   本仓库 `pubspec.yaml` 的 override。
4. 运行 `flutter analyze`、Linux release 构建，并按需运行 Windows/Android 发布构建。
5. 保持内置 libmpv 归档不变，并复验兼容层依赖的两个前提：media_kit 仍通过
   `MPV_RENDER_PARAM_OPENGL_INIT_PARAMS.get_proc_address` 加载 GL 入口，mpv 仍通过
   `eglGetProcAddress` 解析 dmabuf interop 入口；任何 media-kit/mpv 版本变化后重跑
   上文描述的 libmpv 探针。
