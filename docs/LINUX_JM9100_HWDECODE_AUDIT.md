# Linux JM9100（MWV207）硬件解码审查与方案

<!-- audit-markers: jm9100-vaapi-verified; egl-chain-restored-gbm-ok; x11-platform-pending-x-restart; app-hwdec-via-copy-verified; fix-applied-2026-09-08; app-gl-backend-selector-2026-09-10; app-direct-hwdec-blocked-by-bundled-libmpv -->

## 1. 背景与环境

| 项目 | 值 |
| --- | --- |
| 机型架构 | aarch64（deepin 23 系，`Mesa 24.3.0-1deepin9`） |
| 显卡 | 景嘉微 JM9100（`07:00.0 VGA compatible controller`） |
| 内核驱动 | `jmgpu 892928`（mwv207-dkms 1.7.0.uos），`/dev/jmgpu`、`/dev/dri/card0`、`renderD128` 正常 |
| 用户态驱动包 | `com.jingjiamicro.mwv207 1.7.0.uos`、`com.jingjiamicro.mwv207.vaapi 1.6.2.uos` |
| 解码器固件 | JMDEC 1.1.2（驱动自述 `JM jmgpu driver ... Encoder JMENC V6.2 / Decoder JMDEC - 1.1.2`） |
| 视频后端 | mpv 0.40.0（应用经 media_kit 调用）；ffmpeg（系统）、libva 2.19 |
| CPU | 8 核 ARM |

本仓库对应的运行时形态：`lib/player/adapters/media_kit_adapter.dart` 中 Linux 平台 `hwdec = videoHardwareDecoder`（默认 `auto`），`vo` 一律保持平台默认（fork 提交 `76b29e10` 的桌面黑屏修复）。

## 2. 硬解能力验证（已确认可用）

`LIBVA_DRIVER_NAME=jmgpu vainfo`（必须显式指定驱动名，见问题 P4）在 `renderD128` 上成功枚举：

```
VAProfileH264ConstrainedBaseline / H264Main / H264High :
H264MultiviewHigh / H264StereoHigh / JPEGBaseline :
HEVCMain / HEVCMain10 / VP9Profile0               →  VAEntrypointVLD
```

- 直播常用的 **H.264 与 HEVC（含 Main10）位流解码（VLD）全部支持**。
- 无 Encode entrypoint（JMENC 编码器未暴露给 VA-API 入口枚举），影响范围仅限录制/推流，不影响播放硬解。

## 3. 解码性能实测（600 帧 1080p30 H.264 High，testsrc2 4 Mbps，20 s 内容）

| 解码模式 | real | user | sys | 合计 CPU | 说明 |
| --- | --- | --- | --- | --- | --- |
| 软解（`-c:v h264` 默认） | 1.245 s | 4.099 s | 0.159 s | **4.26 s** | 8 核并行软解，吞吐约 17.5x |
| vaapi-copy（解码在 GPU，帧回传内存） | 2.690 s | 0.661 s | 2.488 s | 3.15 s | `sys` 时间极高：GPU→RAM 逐帧搬运（≈700 MB/s）吃掉收益 |
| vaapi-resident（`-hwaccel_output_format vaapi`，帧驻留 GPU） | 1.656 s | **0.238 s** | 0.461 s | **0.70 s** | 解码提交 + 位流解析为唯一 CPU 开销 |

结论：

1. **JMDEC 硬解本身有效**：帧驻留 GPU 模式 CPU 开销为软解的 **16%**（0.70 s vs 4.26 s，约 6 倍降低）。
2. 实时播放 1080p30 时：软解约需 0.21 核持续算力（8 核的 2.6%）；硬解约 0.035 核（0.4%）。
3. 单路 1080p 软解在当前 8 核平台上并不构成实时瓶颈；**硬解的价值在多画面（多视图）、高码率/4K 内容与整机功耗**，且 vaapi-copy 模式因搬运开销反而劣化——应用必须走 direct（帧驻留 GPU → GL 渲染）路径才能兑现收益。
4. mpv 软解实测（vo=gpu，llvmpipe 渲染）单路 1080p30 约 22% 单核（`/tmp/hwdec_bench.sh` 第 1 组 21.8%），与 ffmpeg 数据一致。

## 4. 问题清单（按阻断层级）

### P1（根因）EGL 硬件渲染链路断裂：`libdrm.so.2.4.0` 缺失

- `/usr/lib/aarch64-linux-gnu/mwv207/libEGL_mwv207.so.1.5.0` 的 `DT_NEEDED` 包含 `libdrm.so.2.4.0`（UOS 打包的 libdrm 完整版本文件名），`ldd` 报 `not found`（两处）。
- 当前系统仅安装 `libdrm.so.2 → libdrm.so.2.123.0`（deepin 23 上游版本方案）。
- 后果：mwv207 的 EGL/GLX vendor 无法初始化，glvnd 回落 Mesa → `OpenGL renderer = llvmpipe`（CPU 软件渲染）。
- 系统内存在可用的 `libdrm.so.2.4.0`（aarch64 架构）：
  `/persistent/home/admin/.local/share/containers/storage/overlay/aae79cf37bcf.../diff/usr/lib/aarch64-linux-gnu/libdrm.so.2.4.0`（容器镜像层，i386/x86_64 副本在 deepinwine 下不适用）。

### P2 EGL vendor ICD 的 soname 不匹配

- `/usr/share/glvnd/egl_vendor.d/10_mwv207.json` 指向 `libEGL_mwv207.so.0`，实际文件为 `libEGL_mwv207.so.1.5.0`（目录内仅有 `.so` 与 `.so.1.5.0`）。
- 即便 P1 修复，glvnd 按文件名 `.so.0` 查找仍会失败。

### P3 GLX vendor ICD 缺失

- `/usr/share/glvnd/glx_vendor.d/` 与 `/etc/glvnd/glx_vendor.d/` 均不存在。
- `/etc/profile.d/mwv207_glvnd.sh` 导出的 `__GLX_VENDOR_LIBRARY_NAME=mwv207` 无 ICD 可加载，GLX 路径始终回落 Mesa。X11 下传统 GL 应用（含 mpv `--gpu-context=glx`）无法走硬件。

### P4 运行环境缺少 `LIBVA_DRIVER_NAME=jmgpu`

- libva 1.20 的标准驱动映射不认识 `jmgpu_drv_video.so`，不设置该变量时 `vaGetDriverNames() failed`，VA-API 完全不可用。
- 当前 shell、桌面会话与应用启动环境均未设置。**这是应用内硬解的前置条件**。
- 附带效应：若以 `LD_LIBRARY_PATH` 引入整个 `mwv207/` 目录，目录内 `libgbm.so.1`、`libOpenCL.so` 等通用名库会遮蔽系统库并因 `libdrm.so.2.4.0` 缺失使整个进程启动失败（实测 mpv exit 127）。环境注入必须精确、最小化。

### P5（应用现状）media_kit/mpv 全链路软解

实测证据（`LIBVA_DRIVER_NAME=jmgpu` 已设置）：

- `mpv --vo=gpu --gpu-context=x11egl --hwdec=vaapi`：
  `[vo/gpu/vaapi] VAAPI hwdec only works with OpenGL or Vulkan backends` →
  `[vo/gpu] DR path suspected slow/uncached, disabling` → `[vd] DR failed - disabling`。
- mpv 的 vaapi direct rendering 需要 GL/Vulkan 硬件互操作（EGLImage/dmabuf import）；在 llvmpipe 上 mpv 主动放弃，`hwdec-current = no`。
- 因此**当前应用在 Linux 上完全软解**；仅修复 P4 而不修复 P1–P3 无法改变该结果。

## 5. 修复实施记录（2026-09-08 已执行）

### 系统侧（root）

1. **P1 libdrm 兼容层**（落点与原方案 A 不同，原因见下）：
   - `libdrm.so.2.4.0`（UOS 容器 overlay，aarch64）部署到系统 multiarch 目录
     `/usr/lib/aarch64-linux-gnu/libdrm.so.2.4.0`；
   - **patchelf 将其 SONAME 由 `libdrm.so.2` 改为 `libdrm.so.2.4.0`**。该文件原生
     SONAME 是 `libdrm.so.2`，若不改名会与系统 `libdrm.so.2.123.0` 在 ldconfig cache
     中竞争同一 key（2013 年老库一旦胜出会导致全系统 Mesa/Xorg 符号缺失）；改名后
     两个 soname 各自独立，`ldconfig -p` 验证 `libdrm.so.2 → libdrm.so.2.123.0` 不变。
   - 不按原方案放 `mwv207/` 私有目录的原因：`libEGL_mwv207/libGLX_mwv207/libgbm_jm`
     的 `DT_NEEDED` 写的是字面文件名 `libdrm.so.2.4.0`，运行时目录搜索只覆盖
     `/lib`、`/usr/lib` 等信任目录（私有子目录不在其中，`ld.so.conf.d` 也不按文件名
     索引），私有目录方案必须给每个进程注入 `LD_LIBRARY_PATH` 才能生效——与 P4
     的"最小化注入"原则冲突。改 SONAME 后放系统目录实现全进程零注入生效，且不
     遮蔽任何系统 soname。
2. **P2 EGL ICD**：就地修复 `/usr/share/glvnd/egl_vendor.d/10_mwv207.json`，
   `library_path` 由不存在的 `libEGL_mwv207.so.0` 改为绝对路径
   `/usr/lib/aarch64-linux-gnu/mwv207/libEGL_mwv207.so.1.5.0`。未按原方案另建
   `/etc/glvnd/egl_vendor.d/` 副本（glvnd 同时扫描两处，同一 vendor 重复声明有
   重复注册风险；代价是 mwv207 包升级会覆盖回相对名，升级后需复核）。
3. **P3 GLX ICD**：新建 `/usr/share/glvnd/glx_vendor.d/10_mwv207.json`
   （`name: "mwv207"`，绝对路径指向 `libGLX_mwv207.so.1.2.0`），与 profile.d 已有
   的 `__GLX_VENDOR_LIBRARY_NAME=mwv207` 配对。
4. **P4 会话级**：`/etc/profile.d/mwv207_glvnd.sh` 在 `/dev/jmgpu` 存在条件下追加
   `export LIBVA_DRIVER_NAME=jmgpu`。

### 应用侧（purelive 仓库）

- 方案 B 落地在 `linux/main.cc`：`main()` 最先执行 `jm9100_inject_vaapi_driver_env()`
  —— 仅当 `LIBVA_DRIVER_NAME` 未设置、`/dev/jmgpu` 与
  `/usr/lib/aarch64-linux-gnu/dri/jmgpu_drv_video.so` 都存在时 `setenv`（防御式，
  不污染其他发行版）。放 runner（C++）而非 Dart：media_kit 首次加载 libmpv 前即生效。
- 方案 C：`videoHardwareDecoder` 默认 `auto` 维持不变，`media_kit_adapter.dart`
  无需改动；`vo` 保持平台默认（fork 桌面黑屏修复语义不变）。

### 修复后验证结果

| 项 | 结果 |
| --- | --- |
| `ldd` 三件套（EGL/GLX/gbm_jm） | 全部解析，无 `not found`（P1 解除） |
| `ldconfig -p` | `libdrm.so.2 → 2.123.0` 不变；`libdrm.so.2.4.0` 独立注册 |
| `vainfo --display drm`（LIBVA_DRIVER_NAME=jmgpu） | JMDEC 1.1.2，profile 与第 2 节一致 |
| `eglinfo -B`（X11 + GBM 平台） | `EGL_VENDOR=Jingjia Micro`、`GL_RENDERER=Jingjia JM9100`（OpenGL 4.0 V1.7.0），llvmpipe 链路已断开（P1–P3 生效） |
| mpv `--gpu-context=x11egl --hwdec=vaapi-copy` | exit=0，`hwdec-current=vaapi-copy`（过渡路径可用） |
| mpv `--gpu-context=x11egl --hwdec=no`（基线） | exit=0 |
| `glxinfo -B` | 仍失败，属遗留项（见下），非客户端配置问题 |

### 遗留项（需重启 X / 显示管理器后复验）

`/var/log/Xorg.0.log` 显示**本次修复之前** Xorg 就因 `libdrm.so.2.4.0` 缺失加载专有
X 驱动失败并回落软件 GLX：

```
(EE) Failed to load /usr/lib/xorg/modules/drivers/mwv207_drv.so:
     libdrm.so.2.4.0: cannot open shared object file
(II) GLX: Initialized DRISWRAST GL provider for screen 0
```

1. **重启 X（`sudo systemctl restart lightdm` 或重启机器）后** `mwv207_drv.so`
   应能加载——P1 修复连带治好了 X 驱动加载失败，桌面合成器也将获得 JM9100 硬件
   加速。随后复验：`glxinfo -B`；mwv207 EGL 的 X11 platform 初始化
   （当前会话下 glvnd 在 X11 platform 默认枚举到 Mesa，且强制 mwv207 vendor 时
   `eglInitialize` 失败——与 server 端 swrast 状态一致）；mpv direct 硬解路径；
   第 3 节基准复跑。
2. 复验时注意：当前（X 驱动缺位）状态下，mpv 回落 GBM/DRM context 虽能拿到
   JM9100 GL 4.0，但随后段错误（`V1.7.0` 用户态 GL 在该状态下的稳定性问题）。
   重启后若复现，属 jmgpu 用户态驱动 bug，不属本配置修复范围。

---

## 5A. 原修复方案（存档，与上方实施记录不一致处以实施记录为准）

### 方案 A：补齐 libdrm 兼容层（解决 P1，P2/P3 一并处理）

1. 从容器 overlay 提取 aarch64 的 UOS 版 `libdrm.so.2.4.0`，放置到景嘉微私有目录
   `/usr/lib/aarch64-linux-gnu/mwv207/`（**不放入系统公共目录**，避免污染其他应用）。
2. 新建用户级 EGL vendor json（绝对路径指向真实文件，绕过 soname 问题）：

   ```json
   // /etc/glvnd/egl_vendor.d/10_mwv207.json
   { "file_format_version": "1.0.0",
     "ICD": { "library_path": "/usr/lib/aarch64-linux-gnu/mwv207/libEGL_mwv207.so.1.5.0" } }
   ```

   并补 `/usr/share/glvnd/glx_vendor.d/10_mwv207.json`（指向 `libGLX_mwv207.so.1.2.0`）。
3. 风险：UOS libdrm（2.4.0 命名）与系统 libdrm 2.123 的 ABI 兼容性需以 `ldd` 全链验证 + 实际渲染冒烟确认；libdrm 对内核 DRM ioctl 稳定 ABI 依赖，与 jmgpu 内核驱动（mwv207-dkms 同源 UOS 打包）预期配套，冲突概率低但必须在方案 A 验证步显式覆盖。
4. 若 ABI 冲突：退回仅 symlink `libdrm.so.2.4.0 → libdrm.so.2`（放 mwv207 私有目录 + 精确 `LD_LIBRARY_PATH`），牺牲隔离性换取与系统 libdrm 的一致。

### 方案 B：应用层环境注入（解决 P4，独立可先行）

- 在 Linux 启动脚本/`.desktop`（或 `lib/player/` 启动前注入）设置 `LIBVA_DRIVER_NAME=jmgpu`。
- 精确最小化：只设置该变量；禁止把 `mwv207/` 目录整段加入 `LD_LIBRARY_PATH`（P4 附带效应）。
- 可选防御：环境探测（存在 `/dev/jmgpu` 且 `jmgpu_drv_video.so` 时才注入），避免污染其他发行版。

### 方案 C：应用内解码路径选择（解决 P5 的应用侧部分）

- P1–P3 修复后，mpv 的 `hwdec=auto` 将在 x11egl 上下文选中 `vaapi`（direct，帧驻留 GPU），兑现第 3 节收益。
- 修复前如果需要过渡方案：`hwdec=vaapi-copy` 不依赖 GL 互操作、可独立生效，但按第 3 节数据其收益被搬运开销抵消，**不建议**。
- media_kit `VideoController` 在 Linux 走 libmpv render context → EGL texture（fork `76b29e10` 保证 `vo` 为空），与 vaapi direct 路径天然兼容，无需改 `media_kit_adapter` 代码；`videoHardwareDecoder` 默认 `auto` 即可。

### 验证计划（原计划；第 1、2 步结果见上表，第 3–6 步待重启 X 后执行）

1. `LIBVA_DRIVER_NAME=jmgpu vainfo --display drm --device /dev/dri/renderD128`：profile 枚举不变。
2. `LD_LIBRARY_PATH` 未污染前提下：`glxinfo` renderer 变为非 llvmpipe；`eglinfo`/`mpv -v` EGL_VENDOR 为 mwv207。
3. `mpv --vo=gpu --gpu-context=x11egl --hwdec=auto`：日志出现 `Using hwdec vaapi`（或 `vaapi-egl`），`term-status-msg ${hwdec-current} = vaapi`。
4. 复跑第 3 节基准脚本（vo=gpu 全链路），对比软解 21.8% → 目标显著下降且无 `DR failed` 日志。
5. 应用冒烟：多视图 2×2 H.264 直播间 CPU 对比；HEVC Main10 源抽测。
6. 回归点：桌面渲染切换后确认 fork 桌面修复（`76b29e10`/`f8145711`/`3d2da780`）的行为不变——`vo` 仍为空、输出尺寸策略仍生效。

## 6. 证据文件

- `vainfo` 输出、三模式基准：本节第 2、3 节（原始日志 `/tmp/ff_*.out`、`/tmp/mpv_mesa.log`、`/tmp/mpv_bench_*.log`，临时文件，未入库）。
- 基准脚本：`/tmp/hwdec_bench.sh`、`/tmp/ff_bench.sh`（临时）。
- 本文档为审查结论入库版；2026-09-08 起第 5 节记录修复实施与验证结果，
  第 5A 节为原方案存档。系统改动清单：`/usr/lib/aarch64-linux-gnu/libdrm.so.2.4.0`
  （新增，SONAME 已改名）、`/usr/share/glvnd/egl_vendor.d/10_mwv207.json`（就地修复）、
  `/usr/share/glvnd/glx_vendor.d/10_mwv207.json`（新建）、
  `/etc/profile.d/mwv207_glvnd.sh`（追加 LIBVA_DRIVER_NAME）；
  应用改动：`linux/main.cc`（runner 注入）。

## 7. 修复后复测（2026-09-08，应用与系统环境均为修复后状态）

### 7.1 链路验证

| 验证项 | 结果 |
| --- | --- |
| `vainfo --display drm` | JMDEC 1.1.2 正常，H.264/HEVC profile 完整 |
| `glxinfo` renderer | `llvmpipe` → **`Jingjia JM9100`** |
| mpv `EGL_VENDOR` / `GL_RENDERER` | `Jingjia Micro` / `Jingjia JM9100`（x11egl） |
| `mpv --hwdec=auto` 实际选择 | **`vaapi-copy`**（解码已走 JMDEC；direct 被拒后按序回落） |
| mpv direct（`--hwdec=vaapi`） | 仍被拒：`VAAPI hwdec only works with OpenGL or Vulkan backends` |

### 7.2 整机 CPU 实测（1080p30 H.264，12 s 稳态，8 核）

| 配置 | CPU | 备注 |
| --- | --- | --- |
| vo=null 软解（无渲染，解码基线） | 21.2% | 与修复前 21.8% 一致 |
| vo=gpu 软解 + **llvmpipe** 渲染（补测的修复前形态） | **80.8%** | |
| vo=gpu 软解 + Jingjia GL 渲染 | **74.7%** | 硬件渲染优于 llvmpipe 约 6 个百分点 |
| vo=gpu + vaapi-copy（修复后 auto 实际路径） | **81.2%** | JMDEC 省下的解码 CPU 被 GPU→RAM→GPU 双向拷贝吃回 |
| （ffmpeg 帧驻留 GPU 纯解码，参照） | ≈3.5% 单核等效 | JMDEC 解码器本身效率极高 |

### 7.3 结论

1. 修复达成结构目标：硬件渲染（JM9100）与硬件解码（JMDEC/vaapi-copy）均已生效。
2. **整机播放 CPU 尚未下降**（81.2% vs 修复前 80.8%）：管线成本大头在**渲染路径**（YUV→RGB shader + 呈现，约 50–60%），解码仅约 21%；copy 模式双向搬运抵消解码收益。
3. Jingjia GL（Desktop GL 4.0 / EGL 1.5）渲染比 llvmpipe 略好但 CPU 提交开销仍偏重；direct 零拷贝是兑现收益的最后一块拼图。
4. 应用侧可用手段：fork 的**低内存模式**（`clampVideoOutputToHd`，`3d2da780`）限制输出纹理尺寸，直接降低渲染/采样负载，JM9100 上同样适用；多视图场景建议评估默认开启。

### 7.4 遗留问题（P6）mpv vaapi direct 互操作被拒

- 现象：`hwdec=vaapi`（direct）在 Jingjia EGL（`EGL_EXT_image_dma_buf_import(_modifiers)`、`GL_OES_EGL_image` 均在）上仍报 backend 检查失败，auto 按序回落 vaapi-copy。
- 猜测方向（未定论）：mpv 0.40 direct 模式 backend 判定对私有 GL 栈的兼容性；VA surface 的 DRM modifier/布局是否被 Jingjia dmabuf 导出路径正确声明。
- 建议后续：`--msg-level=vo=v,vd=v` 抓完整探测链 → 对照 mpv 源码 `hwdec_vaapi.c` 判定条件 → 用 `--vd-lavc-software-fallback=no` 定位首个失败点；必要时向景嘉微确认 VA surface dmabuf 导出契约。
- 在 direct 打通前，**应用内硬解（vaapi-copy）不产生整机 CPU 收益**；维持 `videoHardwareDecoder=auto` 即可，收益等待渲染栈与 direct 路径成熟。

## 8. 渲染问题诊断与解决方案（2026-09-08）

### 8.1 定位实验

| 配置（600 帧 1080p30 H.264） | CPU | 含义 |
| --- | --- | --- |
| `vo=vaapi --hwdec=vaapi`（JMDEC 解码 + VA surface 直接呈现，**无 GL**） | **4.9%** | 硬件全管线Works：`Using hardware decoding (vaapi)` |
| `vo=null` 纯软解（无呈现） | 21.2% | 软解成本基线 |
| `vo=gpu` 默认参数（Jingjia GL 渲染） | 74.7% | 渲染层开销 ≈ 53 个百分点 |
| `vo=gpu` 最简参数（bilinear + `--dither-depth=no`） | **36.6%** | 渲染参数可省约 38 个百分点 |

结论：JM9100 的解码与显示硬件管线完全够用（4.9%）；瓶颈按占比排序为
① mpv/GL 渲染参数中的逐像素开销（dither 等高阶 shader，≈38p）；
② Jingjia GL 基础渲染/提交成本与 X11 呈现拷贝（≈15p）；
③ 解码（软解 ≈21p，JMDEC ≈2p）。

### 8.2 方案一（应用侧，立即可做，收益最大）：注入轻量渲染参数

`lib/player/adapters/media_kit_adapter.dart` 在 Linux 平台（`applyNativeLiveProperties` 或
`_configureLinuxCustomOutput` 附近）通过 `native.setProperty(...)` 注入：

| mpv 属性 | 值 | 理由 |
| --- | --- | --- |
| `dither-depth` | `no` | 关闭逐像素 dither shader，Jingjia GL 上开销最大 |
| `scale` / `cscale` | `bilinear` | 避免高阶采样 shader；直播内容无锐化必要 |
| `dither-size-fruit` | `2`（可选） | 若保留 dither，缩小抖动矩阵 |
| `hwdec` | 维持 `videoHardwareDecoder` 语义不变 | 不改变现有设置契约 |

约束与回归点：
- 只对 Linux（`PlatformUtils.isLinux` 分支）注入，Windows/Android 行为不变；
- 预期效果：软解+渲染 74.7% → ≈36.6%；配合 fork 低内存模式（720p 限制）可再降；
- 新增键不需要（渲染参数非用户设置），如需可配可挂到 `player_settings_controller` 但默认值必须覆盖旧安装（策略第 5 节）；
- 回归：`test/media_kit_video_output_configuration_test.dart`、桌面播放冒烟（画质对比 + CPU 复测）。

### 8.3 方案二（系统层，需厂商/系统协作）：X 呈现路径

- 实测 `/var/log/Xorg.0.log`：`mwv207_drv.so` ABI 24（Xorg 1.20）≠ 系统 Xorg ABI 25 → X 驱动加载失败，回落 `modeset(0)`；`DRI2 No driver mapping found for PCI 0x0731/0x9100` → X 端无硬件 GLX（DRISWRAST）。
- 影响：mpv/Flutter 的 GL 渲染结果需经进程间拷贝进入 X pixmap，合成器再采样合成，呈现层多一份带宽与 CPU。
- 措施：向景嘉微索取适配 deepin 23（Xorg ABI 25）的 `mwv207_drv.so`；同步确认桌面合成器进程自身启用 JM9100 GL 加速（否则全屏合成是软件混合）。
- 该项不受应用代码控制，属外部依赖（external-drift）。

### 8.4 方案三（长期，P6 直通路径）

- `vo=vaapi` 实验证明 VA surface 直接呈现可行（4.9%），但 media_kit/Flutter 需要 GL 纹理，必须走 `hwdec=vaapi` direct（EGL dmabuf import）——目前被 mpv backend 检查拒绝（见 7.4）。
- 打通后端到端 ≈ 解码提交 + GL 采样呈现，为理论最优；依赖 Jingjia EGL dmabuf 导出契约与 mpv 兼容性，保持与厂商的跟进。

### 8.5 建议实施顺序

1. 方案一（应用内注入渲染参数）：改动小、无系统依赖、当日可验证；
2. 复测基准（第 7.2 节表格全量重跑）+ 多视图场景抽样；
3. 与厂商并行推进方案二、方案三。

## 9. 启动闪退修复记录（2026-09-08）

### 9.1 现象与根因

驱动修复（EGL/GLX vendor ICD、libdrm）生效后，应用启动即崩溃
（`systemd: Main process exited, code=killed, status=11/SEGV`）。复现与排除：

| 注入组合 | 结果 | 判定 |
| --- | --- | --- |
| 无注入（修复后系统默认） | Impeller OpenGLESSDF → **SEGV** | glvnd 现在成功选中 mwv207 EGL，Jingjia GLES 栈令 Impeller 段错误 |
| 强制 Mesa EGL | 无崩溃但 UI 黑屏 | Mesa 在 X11 上为 PCI 0731:9100 找不到 dri 驱动（`failed to create dri2 screen`）；llvmpipe 回退下 Impeller `Could not determine GL version` |
| 屏蔽全部 EGL vendor | abort（exit 134） | embedder 无自动软件回退 |

与 VAAPI/硬件解码设置无关：崩溃发生在 Flutter surface 创建阶段，先于播放器。

根因：驱动修复让 Jingjia EGL「成功初始化」，把 Flutter 3.47 默认的 Impeller
OpenGLES 后端引入了 Jingjia GLES 栈（段错误）；同时新加的 mwv207 GLX ICD
破坏了 GTK/GLX 路径（`Failed to create OpenGL context: 指定的 RGBA 像素格式
没有可用的设置`），且 `Skia` 壳的 GDK/GLX proc resolver 拿不到 `GL_VERSION`
（FATAL abort）。

### 9.2 修复（应用内，仅 Linux）

- `linux/my_application.cc`：`fl_dart_project_set_enable_impeller(project, FALSE)`
  —— Skia 是 Linux 经典路径，对 GLES2 兼容性远好于 Impeller。
- `linux/main.cc` `jm9100_force_mesa_egl_for_ui()`（仅 `/dev/jmgpu` 存在时注入）：
  - `__EGL_VENDOR_LIBRARY_FILENAMES=50_mesa.json` + `LIBGL_ALWAYS_SOFTWARE=1`
    → UI 走 llvmpipe EGL（Skia），等同驱动修复前的稳定形态；
  - `__GLX_VENDOR_LIBRARY_NAME=mesa`（**覆盖模式**，压过 profile.d 的 mwv207）
    → 修复 GDK/GLX 的 proc resolver。
- `LIBVA_DRIVER_NAME=jmgpu` 注入保持不变：JMDEC 解码与 UI EGL 选择解耦。

### 9.3 验证结果

- 新构建启动：**无 SEGV / FATAL / abort**，Dart 层正常（配置拉取、托盘、
  更新检查均在跑），进程稳定存活。
- 待人工确认：窗口 UI 显示正常后即可交付；播放 CPU 按第 8 节参数预期。

### 9.4 后续项

1. UI 确认后提交：`fix(linux): 禁用 Impeller 并强制 Mesa 软渲染修复 JM9100 启动闪退`。
2. 长期：Jingjia GLES 通过 Flutter 渲染器验证后，可移除 §9.2 注入以恢复硬件 UI 合成；
   建议景嘉微修复 mwv207 GLX ICD 的 RGBA visual 缺失（影响全桌面 GLX 客户端）。

## 10. 驱动直通修复后的应用侧复测（2026-09-10）

驱动侧（`jm9100` 仓库）已完成 VA-API dmabuf 直通修复后，本节记录 purelive 应用侧
的复测结论、已落地的改动与仍未打通的一环。所有结论均来自本机实测，未采信推断。

### 10.1 本轮新增实测事实

| 实测项 | 结果 |
| --- | --- |
| mwv207 GLX ICD 的 FBConfig 集合 | 30 个，visual 为 `0x21/0x22/0x113…0x14c`；**不含**屏幕默认 visual `0x100` |
| GTK/Flutter 默认建窗（visual 0x100） | `gdk_window_create_gl_context` 失败：`指定的 RGBA 像素格式没有可用的设置` → 窗口全黑（mean=0） |
| 指定 visual `0x7c`（本机实测可用值） | GL 上下文创建成功，renderer=`Jingjia JM9100`（GL 4.0） |
| Jingjia EGL（X11 platform） | ES 3.2；`GL_OES_EGL_image`、`EGL_EXT_image_dma_buf_import`、`EGL_KHR_image_base`、`GL_EXT_texture_rg` 均在 |
| Jingjia 桌面 GL 4.0 扩展 | 有 `GL_OES_EGL_image`，**无** `GL_EXT_EGL_image_storage`；`glEGLImageTargetTexture2DOES` 入口存在 |
| 纯 EGL/ES 进程内跑 media_kit 自带 libmpv | `hwdec=vaapi` → `hwdec-current=vaapi`、`Using EGL dmabuf interop via GL_OES_EGL_image`，回读帧为源色红 `(254,24,0)` |
| 同一路径改用 Mesa/llvmpipe（= §9.2 现状） | 仍报 `hwdec-current=vaapi`，但回读帧为全零 NV12 深绿 `(26,130,73)`：**软 GL 下直通静默产出零帧** |
| 进程内先存在 GLX（桌面 GL）上下文后再建 EGL 上下文 | mpv 判定为 `Detected desktop OpenGL 4.0` → `vaapi` 驱动 `VAAPI hwdec only works with OpenGL or Vulkan backends` 加载失败 |

### 10.2 结论

1. **UI 侧的黑屏根因已定位并可绕过**：不是 Impeller、也不是驱动没修好，而是
   `mwv207` GLX vendor 不提供窗口默认 visual（`0x100`）对应的 FBConfig。换用
   vendor 支持的 visual 后，Flutter 引擎可在 `Jingjia JM9100` 上建上下文并正常出图
   （实测窗口 mean≈0.973，无 `Failed to create OpenGL context`）。
2. **应用内直通仍缺一环，且不在内核/驱动**：只要进程里先出现 GTK 的 GLX（桌面 GL）
   上下文，media_kit 的 mpv 上下文就被判定为 desktop GL，而 Jingjia 桌面 GL 未声明
   `GL_EXT_EGL_image_storage`，media_kit 自带（未打补丁）的 libmpv 直接拒绝 VA-API
   interop → 应用内无法进入 direct 硬解。`jm9100` 仓库的
   `mpv_dmabuf_oes_image.patch`（放宽扩展检查、优先走 OES 入口）正是为这一层准备的，
   需要落到应用自带的 `libmpv.so.2` 上。
3. 因此 §9.2 的 Mesa/llvmpipe 注入**暂时仍需保留**：它既是 UI 的稳定形态，也是当前
   唯一"能出画面"的解码路径（代价是 §7.2 的整机 CPU 无收益；若 mpv 在该路径选中
   direct 还可能得到 10.1 表中记录的零帧绿画面——此风险随 libmpv 补丁一并消除）。

### 10.3 本轮落地的应用侧改动

- 新增 `linux/jm9100_gl.{h,cc}`：把原先硬编码的 Mesa 注入收敛为一个可探测、可回退的
  GL 后端选择器，并移出 `main.cc`。
  - 默认（未设置环境变量）：与历史行为完全一致（Mesa EGL + Mesa GLX + `LIBGL_ALWAYS_SOFTWARE`），
    不额外启动任何进程。
  - `PURELIVE_JM9100_GL=hardware`（或 `auto`）：先以子进程自探测（`--jm9100-gl-probe`，
    逐个尝试屏幕 visual，复现 Flutter 的 `gdk_window_create_gl_context` 调用），
    探测成功后保留会话 GL 环境，并把主窗口 visual 设为 vendor 支持的那个；
    探测失败则保持默认 visual 并记录告警。
  - 之所以用"子进程探测 + 主窗口设 visual"，是因为 GL vendor 选择必须发生在任何 GL
    调用之前，而探测本身必然加载 GL 库，不能在主进程里做。
- `linux/my_application.cc`：在窗口 realize 之前应用探测到的 visual（仅硬件模式）。
- `LIBVA_DRIVER_NAME=jmgpu` 注入与"禁用 Impeller"保持不变。
- `lib/player/adapters/media_kit_adapter.dart` 无需改动：`hwdec` 仍为设置项语义
  （默认 `auto`），`vo` 保持平台默认。

### 10.4 验证与证据缺口

验证（`flutter build linux --release` 产物，均截图取证）：

| 运行方式 | 结果 |
| --- | --- |
| 默认（软件）| 日志 `jm9100: using Mesa software rendering …`；窗口 1920x1030，mean≈0.973、无 GL 报错 |
| `PURELIVE_JM9100_GL=hardware` | 日志 `probe visual 0x100: context failed` → `GPU GL stack enabled (visual 0x7c, renderer Jingjia JM9100)` → `window visual set to 0x7c`；窗口正常出图 |
| mpv+EGL/GLES 直通探针（自带 libmpv） | 见 10.1 表：ES 进程内 direct vaapi 正确出图 |

缺口（未取到证据，不作结论）：

1. **应用内播放未采样**：命令行 `--open-room=` 流程在本机无法进入播放页（应用自身
   的 HTTP 通道对 douyu/bilibili 直播接口报 `DioExceptionType.unknown`，shell 侧
   `curl` 同一接口 200，属应用网络/代理配置问题），因此"硬件 GL 栈下应用内播放画面是否
   正常"（含 Jingjia EGL→GLX 的 EGLImage 跨 API 共享）仍为未验证项。
2. 硬件模式下的整机 CPU 与多视图收益未复测（§7.2 表格需在补齐 libmpv 后重跑）。

### 10.5 建议的后续顺序

1. 给应用自带的 `libmpv.so.2` 打上 `jm9100:mpv_dmabuf_oes_image.patch`（当前
   `third_party/media_kit_video/linux/CMakeLists.txt` 直接下载 Predidit 预编译包，
   需替换为带补丁的构建产物），随后验证 mpv 在 desktop GL 下 `hwdec-current=vaapi`。
2. 补齐后把 `PURELIVE_JM9100_GL` 的默认值切到 `auto`（探测通过即用硬件栈），
   并与厂商确认 mwv207 GLX ICD 的默认 visual 缺失问题（影响全桌面 GLX 客户端）。
3. 端到端播放冒烟（单路 + 2×2 多视图 + HEVC Main10）与 §7.2 基准重跑后，再决定是否
   移除 §9.2 的软件渲染回退。
