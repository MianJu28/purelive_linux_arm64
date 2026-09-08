import 'dart:math' as math;

import 'package:flutter/widgets.dart';
import 'package:media_kit/media_kit.dart';

/// Resolves display dimensions from one decoder-parameter snapshot.
///
/// `dw`/`dh` carry sample-aspect-ratio correction when both are available;
/// `w`/`h` are the safe fallback. Rotation is applied exactly once. This
/// policy was previously vendored inside third_party/media_kit_video; after
/// upstream's surface-management refactor removed the geometry helper, it
/// lives here so the adapter keeps a single, unit-testable geometry source.
({int width, int height})? resolveVideoParamsDisplaySize(VideoParams params) {
  final correctedWidth = params.dw;
  final correctedHeight = params.dh;
  final rawWidth = params.w;
  final rawHeight = params.h;

  final hasCorrectedPair =
      correctedWidth != null && correctedWidth > 0 && correctedHeight != null && correctedHeight > 0;
  final hasRawPair = rawWidth != null && rawWidth > 0 && rawHeight != null && rawHeight > 0;
  if (!hasCorrectedPair && !hasRawPair) return null;

  late final int decodedWidth;
  late final int decodedHeight;
  if (hasCorrectedPair) {
    decodedWidth = correctedWidth;
    decodedHeight = correctedHeight;
  } else {
    decodedWidth = rawWidth!;
    decodedHeight = rawHeight!;
  }
  final rotation = (((params.rotate ?? 0) % 360) + 360) % 360;
  final quarterTurn = rotation == 90 || rotation == 270;
  return quarterTurn
      ? (width: decodedHeight, height: decodedWidth)
      : (width: decodedWidth, height: decodedHeight);
}

/// Chooses the smallest native video texture that fully covers the visible
/// viewport without upscaling beyond the decoded source dimensions.
Size calculateVideoOutputSize({
  required Size logicalViewport,
  required double devicePixelRatio,
  int? sourceWidth,
  int? sourceHeight,
}) {
  if (!logicalViewport.width.isFinite ||
      !logicalViewport.height.isFinite ||
      logicalViewport.isEmpty ||
      !devicePixelRatio.isFinite ||
      devicePixelRatio <= 0) {
    return Size.zero;
  }

  final viewportWidth = logicalViewport.width * devicePixelRatio;
  final viewportHeight = logicalViewport.height * devicePixelRatio;
  final validSource =
      sourceWidth != null && sourceWidth > 0 && sourceHeight != null && sourceHeight > 0;
  // Use a conservative 1080p provisional source before mpv publishes video
  // parameters. It is replaced immediately when the real dimensions arrive.
  final source = validSource
      ? Size(sourceWidth.toDouble(), sourceHeight.toDouble())
      : const Size(1920, 1080);
  final scale = math.min(
    1.0,
    math.min(viewportWidth / source.width, viewportHeight / source.height),
  );

  int evenPixel(double value) {
    final rounded = math.max(2, value.round());
    return rounded.isEven ? rounded : rounded + 1;
  }

  return Size(
    evenPixel(source.width * scale).toDouble(),
    evenPixel(source.height * scale).toDouble(),
  );
}

/// Caps a resolved output texture at an HD short-side ceiling, preserving the
/// aspect ratio and even dimensions.
///
/// On hosts where software decoding, mpv rendering and Flutter compositing
/// share one CPU (software GL rasterizers such as llvmpipe), every texture
/// pixel is paid for on the CPU three times. Pinning the texture to HD is the
/// most effective frame-rate lever there; it is opt-in through low-memory
/// mode so normal hosts keep full resolution.
Size clampVideoOutputToHd(Size size, {int maxShortSide = 720}) {
  if (size.isEmpty || size.shortestSide <= maxShortSide || maxShortSide <= 0) {
    return size;
  }

  final scale = maxShortSide / size.shortestSide;

  int evenPixel(double value) {
    final rounded = math.max(2, value.round());
    return rounded.isEven ? rounded : rounded + 1;
  }

  return Size(evenPixel(size.width * scale).toDouble(), evenPixel(size.height * scale).toDouble());
}
