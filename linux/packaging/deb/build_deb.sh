#!/usr/bin/env bash
#
# 构建 Linux deb 安装包（支持 arm64 / x64）
#
# 用法:
#   ./linux/packaging/deb/build_deb.sh [架构]
#
# 架构默认从当前运行环境自动检测 (uname -m)，也可显式传入:
#   ./linux/packaging/deb/build_deb.sh arm64
#   ./linux/packaging/deb/build_deb.sh x86_64
#
# 前置条件:
#   1. 已执行 `flutter build linux --release`，产物位于 build/linux/<arch>/release/bundle
#   2. 系统已安装 dpkg-deb（Debian/Ubuntu 自带）
#
# 输出: dist/PureLive-<version>-linux-<arch>.deb

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# ---------- 参数解析 ----------
if [ "$#" -ge 1 ]; then
  ARCH_INPUT="$1"
else
  case "$(uname -m)" in
    aarch64|arm64) ARCH_INPUT=arm64 ;;
    x86_64|amd64)  ARCH_INPUT=x86_64 ;;
    *) echo "不支持的架构: $(uname -m)" >&2; exit 1 ;;
  esac
fi

case "$ARCH_INPUT" in
  arm64|aarch64)  DEB_ARCH=arm64; FLUTTER_TRIPLE=arm64 ;;
  x86_64|amd64)   DEB_ARCH=amd64; FLUTTER_TRIPLE=x64   ;;
  *) echo "不支持的架构: $ARCH_INPUT" >&2; exit 1 ;;
esac

# ---------- 读取版本 ----------
VERSION="$(grep '^version:' "${PROJECT_DIR}/pubspec.yaml" | sed 's/version: *//; s/[`"'"'"']//g' | head -n1)"
# deb 版本号不允许出现 '+' ，替换为 '~'
DEB_VERSION="${VERSION/+/\~}"

APP_NAME="pure_live"
APP_DISPLAY_NAME="纯粹直播"
APP_ID="com.example.pure_live"
ICON_SRC="${PROJECT_DIR}/assets/icons/icon.png"

BUNDLE_DIR="${PROJECT_DIR}/build/linux/${FLUTTER_TRIPLE}/release/bundle"
OUTPUT_DIR="${PROJECT_DIR}/dist"
OUTPUT_FILE="${OUTPUT_DIR}/${APP_NAME}-${DEB_VERSION}-linux-${DEB_ARCH}.deb"

# ---------- 校验 ----------
if [ ! -d "${BUNDLE_DIR}" ]; then
  echo "错误: 未找到 Flutter Linux 构建产物 ${BUNDLE_DIR}" >&2
  echo "请先执行: flutter build linux --release" >&2
  exit 1
fi
if [ ! -f "${ICON_SRC}" ]; then
  echo "错误: 未找到应用图标 ${ICON_SRC}" >&2
  exit 1
fi
if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "错误: 未找到 dpkg-deb，请安装 dpkg 工具" >&2
  exit 1
fi

# ---------- 组装 deb 目录结构 ----------
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

PKG_ROOT="${STAGE_DIR}/pkg"
OPT_DIR="${PKG_ROOT}/opt/${APP_NAME}"
BIN_DIR="${PKG_ROOT}/usr/bin"
APP_DIR="${PKG_ROOT}/usr/share/applications"
ICON_DIR="${PKG_ROOT}/usr/share/icons/hicolor/512x512/apps"
METAINFO_DIR="${PKG_ROOT}/usr/share/metainfo"

mkdir -p "${OPT_DIR}" "${BIN_DIR}" "${APP_DIR}" "${ICON_DIR}" "${METAINFO_DIR}"

# 应用本体（Flutter bundle）放到 /opt/pure_live
cp -a "${BUNDLE_DIR}/." "${OPT_DIR}/"

# 可执行文件软链接到 /usr/bin
ln -s "/opt/${APP_NAME}/${APP_NAME}" "${BIN_DIR}/${APP_NAME}"

# .desktop 桌面入口
cat > "${APP_DIR}/${APP_NAME}.desktop" <<EOF
[Desktop Entry]
Name=${APP_DISPLAY_NAME}
Name[en]=${APP_NAME}
Comment=一款开源的第三方多平台直播聚合播放器
Exec=${APP_NAME}
Icon=${APP_NAME}
Terminal=false
Type=Application
Categories=AudioVideo;Player;Network;
StartupNotify=true
EOF

# 应用图标
cp "${ICON_SRC}" "${ICON_DIR}/${APP_NAME}.png"

# AppStream metainfo
cat > "${METAINFO_DIR}/${APP_ID}.metainfo.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>${APP_ID}</id>
  <name>${APP_DISPLAY_NAME}</name>
  <summary>一款开源的第三方多平台直播聚合播放器</summary>
  <categories>
    <category>AudioVideo</category>
    <category>Player</category>
  </categories>
</component>
EOF

# ---------- 计算安装大小 ----------
INSTALLED_SIZE="$(du -sk "${OPT_DIR}" | cut -f1)"

# ---------- 生成 control 文件 ----------
DEBIAN_DIR="${PKG_ROOT}/DEBIAN"
mkdir -p "${DEBIAN_DIR}"
cat > "${DEBIAN_DIR}/control" <<EOF
Package: ${APP_NAME}
Version: ${DEB_VERSION}
Section: x11
Priority: optional
Architecture: ${DEB_ARCH}
Installed-Size: ${INSTALLED_SIZE}
Maintainer: PureLive <purelive@example.com>
Description: ${APP_DISPLAY_NAME}
 一款开源的第三方多平台直播聚合播放器，支持 Bilibili、抖音、斗鱼、
 虎牙、快手等多个平台的直播源聚合播放。
EOF

# ---------- 打包 ----------
mkdir -p "${OUTPUT_DIR}"
dpkg-deb --build --root-owner-group "${PKG_ROOT}" "${OUTPUT_FILE}"

echo "✅ 已生成 deb 包: ${OUTPUT_FILE}"