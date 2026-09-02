import 'package:flutter_test/flutter_test.dart';
import 'package:media_kit_video/media_kit_video.dart';
import 'package:pure_live/player/adapters/media_kit_adapter.dart';

void main() {
  group('resolveVideoOutputDriver', () {
    test('never overrides vo on desktop and iOS', () {
      // Windows, GNU/Linux, macOS and iOS render through media_kit's `libmpv`
      // render context. Forwarding the stored Android renderer there detaches
      // mpv from that context, which shows up as sound without a picture.
      for (final driver in <String>['', 'auto', 'gpu', 'gpu-next', 'mediacodec_embed', 'libmpv']) {
        expect(
          resolveVideoOutputDriver(android: false, configuredDriver: driver, androidSdkInt: 34),
          isNull,
          reason: 'desktop/iOS must keep the platform default for "$driver"',
        );
      }
    });

    test('keeps the platform default for the stored default renderer', () {
      // 'gpu' is the shipped default of [videoOutputDriver]. It is valid
      // Android vocabulary but breaks the Linux texture output.
      expect(resolveVideoOutputDriver(android: false, configuredDriver: 'gpu', androidSdkInt: 33), isNull);
    });

    test('maps the automatic Android selection by SDK level', () {
      expect(resolveVideoOutputDriver(android: true, configuredDriver: 'auto', androidSdkInt: 33), 'gpu');
      expect(resolveVideoOutputDriver(android: true, configuredDriver: 'auto', androidSdkInt: 34), 'gpu-next');
      expect(resolveVideoOutputDriver(android: true, configuredDriver: '', androidSdkInt: 34), 'gpu-next');
    });

    test('honours an explicit Android renderer', () {
      expect(resolveVideoOutputDriver(android: true, configuredDriver: 'gpu-next', androidSdkInt: 33), 'gpu-next');
      expect(
        resolveVideoOutputDriver(android: true, configuredDriver: 'mediacodec_embed', androidSdkInt: 34),
        'mediacodec_embed',
      );
    });
  });

  group('resolveVideoControllerConfiguration', () {
    VideoControllerConfiguration resolve(
      PlayerHostPlatform platform, {
      bool customPlayerOutput = true,
      bool playerCompatMode = false,
      String videoOutputDriver = 'gpu',
      String videoHardwareDecoder = 'auto',
      bool enableRtxVsr = false,
      bool enableCodec = true,
      int androidSdkInt = 33,
    }) {
      return resolveVideoControllerConfiguration(
        platform: platform,
        customPlayerOutput: customPlayerOutput,
        playerCompatMode: playerCompatMode,
        videoOutputDriver: videoOutputDriver,
        videoHardwareDecoder: videoHardwareDecoder,
        enableRtxVsr: enableRtxVsr,
        enableCodec: enableCodec,
        androidSdkInt: androidSdkInt,
      );
    }

    test('keeps the libmpv default on every non-Android host', () {
      // Regression: an enabled "custom output" used to forward the Android
      // renderer list to mpv, which produced audio with a black picture while
      // multiview (which never sets `vo`) kept rendering normally.
      for (final platform in <PlayerHostPlatform>[
        PlayerHostPlatform.linux,
        PlayerHostPlatform.windows,
        PlayerHostPlatform.macOS,
        PlayerHostPlatform.iOS,
        PlayerHostPlatform.other,
      ]) {
        expect(resolve(platform).vo, isNull, reason: '${platform.name} must not override vo');
      }
    });

    test('still honours the decoder settings without custom output', () {
      final config = resolve(PlayerHostPlatform.linux, customPlayerOutput: false);

      expect(config.vo, isNull);
      expect(config.hwdec, isNull);
      expect(config.enableHardwareAcceleration, isTrue);
    });

    test('uses the Android renderer only on Android', () {
      expect(resolve(PlayerHostPlatform.android).vo, 'gpu');
      expect(resolve(PlayerHostPlatform.android, videoOutputDriver: 'gpu-next').vo, 'gpu-next');
    });

    test('keeps the Android compatibility renderer', () {
      final config = resolve(PlayerHostPlatform.android, playerCompatMode: true);

      expect(config.vo, 'mediacodec_embed');
      expect(config.hwdec, 'mediacodec');
      expect(config.enableHardwareAcceleration, isTrue);
    });

    test('resolves the hardware decoder per platform', () {
      expect(resolve(PlayerHostPlatform.linux, videoHardwareDecoder: 'vaapi').hwdec, 'vaapi');
      expect(resolve(PlayerHostPlatform.linux, videoHardwareDecoder: '').hwdec, 'auto');
      expect(resolve(PlayerHostPlatform.windows, enableRtxVsr: true).hwdec, 'd3d11va');
      expect(resolve(PlayerHostPlatform.windows, videoHardwareDecoder: 'd3d11va-copy').hwdec, 'd3d11va-copy');
      expect(resolve(PlayerHostPlatform.macOS, videoHardwareDecoder: 'videotoolbox').hwdec, 'no');
    });

    test('disables hardware acceleration on macOS only', () {
      expect(resolve(PlayerHostPlatform.macOS).enableHardwareAcceleration, isFalse);
      expect(resolve(PlayerHostPlatform.linux).enableHardwareAcceleration, isTrue);
      expect(resolve(PlayerHostPlatform.linux, enableCodec: false).enableHardwareAcceleration, isFalse);
    });
  });
}
