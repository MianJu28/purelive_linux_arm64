# 上游同步审查：4624d389 → 4475550d

- 冻结上游提交：`4475550d47be719ff1cd6a885c46165f05cb6ef7`（2026-09-02，`feat: 简化硬件解码器设置逻辑，移除默认值处理`）
- 合并基点：`4624d389f0eee1f85b6018db9727cf899c6c1b3d`
- 入站范围：`4624d389..4475550d`，共 20 个提交（含 2 个上游内部 merge），57 个文件，+9206/−1454
- 审查方式：本机无 pwsh，`tool/review_upstream_update.ps1` 不可执行；按策略手动完成等效的冻结、三方比较、逐文件分类与处置。`local-artifacts/upstream-reviews/` 的机器证据由 GitHub Actions `Audit Upstream Update` 工作流补充生成。
- 三方比较：
  - `merge-base..upstream`：上游行为（见台账）
  - `merge-base..fork`：播放器黑屏/帧率修复（`c5276485`/`76b29e10`/`ad2033a9`/`3e9ba1af`）、弹幕重绘隔离（`3ec28ca1`）、Linux arm64 构建适配与 PingFang 字体
  - 候选结果：见「fork_feature_impact」与「conflict_resolution」

## file_review

| 类别 | 文件 | 处置 |
| --- | --- | --- |
| 播放器核心（fork 交集） | `lib/player/adapters/media_kit_adapter.dart` | adapt：接受 `PlaybackCachePolicy`，保留 fork 平台 vo 门禁与输出尺寸固定 |
| 播放器核心（fork 交集） | `lib/player/global_player_service.dart` | accept：`initialize()` 改为 `configureDefaultEngine()` 惰性启动 |
| 播放器核心（fork 交集） | `lib/player/utils/player_consts.dart` | adapt：接受跨平台 `videoRenderersList`；fork 端仍仅在 Android 下发 `vo` |
| 播放器核心 | `lib/player/utils/playback_cache_policy.dart`（新增） | accept：动态缓存策略，替代 fork 静态 `LiveBufferPolicy`（见台账 #6） |
| 设置（fork 交集） | `lib/common/services/settings/player_settings_controller.dart` | adapt：接受 `videoOutputDriver` 默认 `auto`；保留 `defaultSuperResolutionMode` 独立存储键 |
| 设置 | `lib/common/services/settings/cookie_settings_controller.dart`、`settings_service.dart` | accept |
| 播放页 UI（fork 交集） | `lib/modules/live_play/widgets/video_player/video_controller_panel.dart` | accept：全屏流选择面板重构与 fork 弹幕 `RepaintBoundary` 无重叠，自动合并后复核共存 |
| 播放页 UI | `lib/modules/live_play/pages/super_chat_page.dart`、`widgets/danmaku/danmaku_tab.dart` | accept |
| 账户/Cookie | `lib/core/common/cookie_string.dart`（新增）、`lib/modules/account/cookie_validator.dart`（新增）、`lib/modules/account/web_cookie_capture.dart`（新增） | adapt：Linux 无 WebView 实现，补平台降级提示（见台账 #7） |
| 账户/Cookie | douyin/huya/kuaishou/soop/twitch 各 `*_cookie_controller.dart`/`*_cookie_page.dart` | accept |
| 多画面 | `lib/modules/multiview/multiview_page.dart`、`multiview_room_search_controller.dart`（新增）、`widgets/multiview_room_picker.dart`、`widgets/multiview_room_search_panel.dart`（新增） | accept |
| 房间卡片设置 | `lib/modules/settings/pages/room_card_settings/*`（8 个新增文件） | accept |
| 设置页 | `player_kernel_settings_page.dart`、`renderer_settings.dart`、`theme_settings_page.dart` | accept |
| 通用组件 | `common_avatar.dart`、`menu_button.dart`、`pure_live_scroll_physics.dart`、`room_card.dart`、`widget_extensions.dart`、`routes/android_native_page_transition.dart` | accept |
| 首页/收藏 | `lib/main.dart`、`area_rooms_page.dart`、`favorite_controller.dart`、`room_grid_view.dart`、`popular_grid_view.dart`、`remote_sync_page.dart`、`common/consts/app_consts.dart` | accept |
| 资源 | `assets/images/avatar.jpg`（新增二进制） | accept |
| 翻译 | `assets/translations/en.json`、`zh.json` | adapt：新增键 `cookie_capture_unsupported`（zh/en），其余接受 |
| 测试 | `test/multiview_room_search_panel_test.dart`（新增） | adapt：修复上游带入的 `prefer_initializing_formals` info |
| 测试 | `test/cover_metric_badge_test.dart`（删除） | accept：对应功能已移除 |

无凭据形态字符串、无可变 Git/Action 引用、无 `pull_request_target`、无 `permissions: write-all`、无手动构建默认开启（人工逐文件核对 + `audit_repository.py` 复扫）。

## semantic_change_ledger

| # | commit/file | upstream intent | issue_and_bug_mapping | implementation | quality_assessment | fork_feature_impact | disposition |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `66276ae4` `player_consts.dart`/`renderer_settings.dart`/`player_kernel_settings_page.dart` | 渲染器列表从 Android 专用开放为跨平台（新增 vaapi/vdpau/drm/x11 等），`videoOutputDriver` 默认 `gpu`→`auto` | 无关联 Issue；属功能开放 | 列表重命名 `videoRenderersList` 并扩项；UI 显示条件调整 | 列表本身正确；但**在 media_kit 桌面端下发任意非 `libmpv` 的 `vo` 会脱离 render context 导致黑屏**（fork `c5276485`/`76b29e10` 的根因），上游列表不含 `libmpv`，桌面端逐项选择均会复现黑屏 | fork 的 `resolveVideoOutputDriver` 门禁保留：桌面/iOS 一律不下发 `vo`，仅 Android 生效。UI 候选列表随上游更新，但桌面端不渲染该入口 | adapt |
| 2 | `87941dea`/`4475550d` `player_settings_controller.dart`/`media_kit_adapter.dart` | 硬件解码器设置简化：`videoHardwareDecoder` 不再补默认值，直传 mpv | 无关联 Issue | `hwdec = settings.videoHardwareDecoder.v`（移除空值兜底） | mpv 对 `hwdec=` 空串按 auto 处理，行为等价 | fork 兼容：空值仍由 `resolveVideoControllerConfiguration` 归一为 `auto` | accept |
| 3 | `2b68e823` 房间刷新 | 刷新增加超时与错误日志，避免无限等待 | 无关联 Issue | 刷新链路加 timeout + 日志 | 合理 | 无 | accept |
| 4 | `c1644fe5`/`7a0fb2e8`/`edc41c2a`/`d9a9c124`/`8c9e4491`/`d86d3133`/`5df533f5` 房间卡片设置模块 | 新增房间卡片高度/预设/调试渲染配置（RoomCardConfigController 等 8 文件） | 无关联 Issue | Hive 新键 + 设置页 + 渲染器 | 结构清晰；新键均有默认值 | 新设置模块，不影响 fork 播放器路径 | accept |
| 5 | `9b7b2bb0`/`b06726a0` multiview 搜索与 Cookie | 多画面搜索选台面板 + 平台 Cookie 自动校验/抓取 | 上游 PR #831 | 新控制器/面板 + CookieValidator + WebCookieCapturePage | 校验仅覆盖 douyin/twitch，其余 unverified 合理 | Cookie 抓取页依赖 `flutter_inappwebview`；fork Linux 构建已剔除其平台实现（`a5dc580d`），需降级（见 #7） | adapt |
| 6 | `66276ae4` 等 `playback_cache_policy.dart`（新增） | 引入动态缓存策略：`lowMemoryMode`/计费网络感知，默认 1500MiB/60s，低内存 64MiB/10s | 无关联 Issue | 替代 fork 静态 `LiveBufferPolicy`（32MiB/2s）的调用点；`startWatching()` 即时 apply | 机制更完备（动态响应），但默认值远大于 fork 低延迟直播策略，延迟与内存上限行为变化 | fork `LiveBufferPolicy` 成为孤儿（仅测试引用）；mpv 缓冲由上游策略主导。**已评估 accept**：直播 readahead 为预读目标而非强制延迟，且有 lowMemoryMode 兜底；回滚点为合并提交本身 | accept |
| 7 | `9b7b2bb0` `web_cookie_capture.dart` | 内置浏览器抓取平台 Cookie | 上游 PR #831 | 全屏 InAppWebView + 域名过滤 + 校验链路 | Windows 崩溃路径已有防护；**Linux 缺平台实现会 MissingPluginException** | 补 `_webViewAvailable` 平台守卫：Linux 降级为提示页并新增 `cookie_capture_unsupported` 翻译键；其余平台行为不变 | adapt |
| 8 | `e9108be4` `video_controller_panel.dart` | 全屏流选择面板重构（桌面/移动适配） | 无关联 Issue | 对话框布局重写 | 与 fork 弹幕改动不同区域，自动合并后复核共存 | 无语义冲突 | accept |
| 9 | `9eae6bf6` `pure_live_scroll_physics.dart` 等 | 平台特定滚动物理 | 无关联 Issue | 滚动物理抽象 | 合理 | fork 已有 `desktop_scroll_behavior` 测试覆盖路径不变 | accept |
| 10 | `2d3e51e3` `menu_button.dart` | 菜单按钮增加备份恢复入口 | 无关联 Issue | 菜单项扩展 | 合理 | 无 | accept |
| 11 | `57a3d487` 播放器资源释放提示 | 退出提示/设置选项 | 无关联 Issue | 提示与设置 | 新增设置键有默认值 | 无 | accept |
| 12 | `fc248d65` `player_kernel_settings_page.dart` | 标题构建方式与间距调整 | 无关联 Issue | UI 重构 | 合理 | 无 | accept |
| 13 | `global_player_service.dart` | 播放器惰性初始化（`configureDefaultEngine`） | 无关联 Issue | 启动不创建播放器 | 降低启动内存/CPU，与 fork `_syncNativeOutputSize`（init 时调用）无时序冲突 | 首次进房间时间可能略增 | accept |
| 14 | `8da2b757` `cover_metric_badge_test.dart` 删除 | 移除失效测试 | 无关联 Issue | 删除 | 与 `cover_metric_badge.dart` 源码保留与否核对：源码仍在仓库（`lib/common/widgets/`），测试删除后该组件无覆盖 | 记录覆盖缺口 | accept |
| 15 | `04883a74`/`b06726a0` merge 提交 | 上游内部整合 | — | — | — | 保留祖先关系 | accept |

## issue_and_bug_mapping

上游本批次无引用 Issue 编号的修复；唯一外部关联为 PR #831（multiview 搜索/Cookie）。fork 侧对应判定：无新映射 Bug；fork 的黑屏/帧率修复继续覆盖上游未感知的桌面纹理路径（见 #1）。

## fork_feature_impact

- 保留：桌面/iOS 不下发 `vo`（黑屏修复）、单画面输出尺寸固定（帧率修复）、`defaultSuperResolutionMode` 独立键、弹幕 `RepaintBoundary`、`TickerMode`、Linux arm64 构建适配（工作流/CMake/deb/PingFang）。
- 变化：mpv 缓冲改由 `PlaybackCachePolicy` 主导（默认 1500MiB/60s，低内存 64MiB/10s）；渲染器设置页候选列表跨平台化但桌面端入口维持 fork 门禁；Cookie 抓取在 Linux 降级为提示。

## quality_assessment

上游代码质量总体可接受；三处需修正的点均已在本合并中处理：桌面 `vo` 黑屏路径（保留 fork 门禁）、Linux WebView 缺实现（降级守卫）、测试 info lint（initializing formal）。

## conflict_resolution

- 唯一文本冲突：`lib/player/adapters/media_kit_adapter.dart`（3 处块）。
  - imports：取上游 `playback_cache_policy.dart` + fork `video_output_size_policy.dart`；删除孤儿 `live_buffer_policy.dart` import（类本体与测试保留）。
  - `_buildVideoControllerConfiguration`：取 fork 的纯函数解析架构，注释补充上游默认 `auto` 的桌面黑屏风险说明；上游的平台分支实现丢弃（其桌面 `vo` 下发即黑屏根因）。
- 自动合并成功但经语义复核确认的文件：`player_settings_controller.dart`（默认 `auto` 与 fork 超分键共存）、`video_controller_panel.dart`（面板重构与 `RepaintBoundary` 共存）、`media_kit_adapter.dart` 的 `PlaybackCachePolicy` 调用点与 `_syncNativeOutputSize` 共存。

## regression_plan

- 受影响测试：`media_kit_video_output_configuration_test`（vo 门禁）、`player_settings_controller_test`（超分键）、`live_buffer_policy_test`（孤儿常量仍成立）、`multiview_room_search_panel_test`（上游新增）、播放/弹幕相关测试、翻译 JSON 合法性。
- 平台矩阵：Linux arm64（本机重点）、Android（`vo` 行为唯一受影响平台）、Windows（面板重构 + Cookie 抓取）静态验证；真机采样未覆盖，作为证据缺口记录。
- 失败回滚点：合并提交 `git revert -m 1 <merge>`。

## verification_plan

1. `git diff --check` 无空白错误。
2. `flutter analyze --no-pub --no-fatal-infos --no-fatal-warnings` 通过（已通过）。
3. `python tool/audit_repository.py` 全仓复扫（合并后执行）。结果：22 error/1 warning，全部集中于 fork 既有工作流 `linux-arm64.yml`（可变 action 引用 v4、mutable git clone、未锁 pub get）与 `build_pure_live_release.yml`（手动输入 default true）；本次合并未改动任何 `.github/workflows/` 文件（`git diff HEAD --name-only` 零命中），故无合并引入的违规。两项为 fork 存量构建流水线维护债，不阻塞本同步，记入后续维护范围。
4. `flutter test` 播放/弹幕/设置/翻译相关套件（见 regression_plan）。
5. 正式发布另行走 `BUILD_POLICY.md` 完整门禁，不在本次同步范围。
