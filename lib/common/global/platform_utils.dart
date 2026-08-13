import 'dart:io' show Platform;
import 'package:flutter/material.dart';

class PlatformUtils {
  PlatformUtils._();
  static bool get isDesktop => Platform.isWindows || Platform.isLinux || Platform.isMacOS;

  static bool get isDesktopNotMac => (Platform.isWindows || Platform.isLinux) && !Platform.isMacOS;

  static bool get isMobile => Platform.isAndroid || Platform.isIOS;

  static bool isMobileWidth(BuildContext context) {
    return MediaQuery.of(context).size.width < 760;
  }

  static bool get isWindows => Platform.isWindows;
  static bool get isMacOS => Platform.isMacOS;
  static bool get isLinux => Platform.isLinux;
  static bool get isAndroid => Platform.isAndroid;
  static bool get isIOS => Platform.isIOS;

  static T select<T>({required T desktop, required T mobile}) {
    return isDesktop ? desktop : mobile;
  }

  /// 弹幕默认字体的真实字体族。
  ///
  /// 弹幕由 flame_barrage 用 dart:ui 的 ParagraphBuilder 渲染，不会继承
  /// Flutter 主题字体。Linux 精简系统缺 CJK 字体，若传 null 会导致中文显示
  /// 为方框；应用已打包完整中文字体 PingFangSC.ttf（family=PingFang），
  /// 故 Linux/Windows 返回 'PingFang'，其余平台返回 null 走引擎默认。
  static String? resolveDefaultDanmakuFontFamily() {
    if (isWindows || isLinux) return 'PingFang';
    return null;
  }
}
