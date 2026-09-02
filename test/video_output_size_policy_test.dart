import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pure_live/player/utils/video_output_size_policy.dart';

void main() {
  test('caps textures at an HD short-side ceiling for software rasterizers', () {
    // 1440p host texture resolved for a 1440p source on a 2K display.
    final clamped = clampVideoOutputToHd(const Size(2560, 1440));

    expect(clamped.shortestSide, 720);
    expect(clamped.aspectRatio, closeTo(2560 / 1440, 0.01));
    expect(clamped.width.toInt().isEven, isTrue);
    expect(clamped.height.toInt().isEven, isTrue);

    // Sub-HD sizes pass through untouched.
    expect(clampVideoOutputToHd(const Size(1280, 720)), const Size(1280, 720));
    expect(clampVideoOutputToHd(const Size(640, 360)), const Size(640, 360));
    expect(clampVideoOutputToHd(Size.zero), Size.zero);
  });

  test('uses physical viewport pixels without upscaling the source', () {
    expect(
      calculateVideoOutputSize(
        logicalViewport: const Size(1280, 720),
        devicePixelRatio: 1.5,
        sourceWidth: 3840,
        sourceHeight: 2160,
      ),
      const Size(1920, 1080),
    );
    expect(
      calculateVideoOutputSize(
        logicalViewport: const Size(3840, 2160),
        devicePixelRatio: 1,
        sourceWidth: 1920,
        sourceHeight: 1080,
      ),
      const Size(1920, 1080),
    );
  });

  test('preserves aspect ratio and even texture dimensions', () {
    final size = calculateVideoOutputSize(
      logicalViewport: const Size(500, 500),
      devicePixelRatio: 1,
      sourceWidth: 1920,
      sourceHeight: 1080,
    );

    expect(size, const Size(500, 282));
    expect(size.width.toInt().isEven, isTrue);
    expect(size.height.toInt().isEven, isTrue);
  });

  test('uses a bounded provisional size until source metadata arrives', () {
    expect(
      calculateVideoOutputSize(logicalViewport: const Size(3840, 2160), devicePixelRatio: 1),
      const Size(1920, 1080),
    );
    expect(calculateVideoOutputSize(logicalViewport: Size.zero, devicePixelRatio: 1), Size.zero);
  });
}
