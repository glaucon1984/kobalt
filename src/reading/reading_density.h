#pragma once
#include <QtGlobal>
#include <atomic>

namespace kobalt {
// The reading screen's pixels per inch. The engine lays out text in device
// pixels, so reading sizes convert here. Each host sets it from its screen;
// the interface keeps its own copy for UI sizes.
inline std::atomic<int> readingPpi{300};

// Reading sizes are designed at 300 ppi, like the UI, and keep their physical
// size on other screens.
inline int readingPixels(int design) {
    return qRound(design * readingPpi.load() / 300.0);
}
inline int defaultFontPixels() {
    return qRound(11 * readingPpi.load() / 72.0);
}
inline int minimumFontPixels() {
    return qRound(6 * readingPpi.load() / 72.0);
}
inline int maximumFontPixels() {
    return qRound(48 * readingPpi.load() / 72.0);
}
inline int defaultMarginPixels() {
    return readingPixels(48);
}
inline int maximumMarginPixels() {
    return readingPixels(120);
}
inline int defaultVerticalMarginPixels(int sideMargin) {
    return qMin(sideMargin, readingPixels(24));
}

// The text size control counts half points, so a size reads the same on every
// screen: 21 is 10.5 pt, 44 pixels at 300 ppi. The range stays inside the pixel
// limits above on every density.
inline constexpr int minimumHalfPoints = 12, maximumHalfPoints = 96;
inline int halfPointPixels(int halfPoints) {
    return qRound(halfPoints * readingPpi.load() / 144.0);
}
inline int pixelHalfPoints(int pixels) {
    return qRound(pixels * 144.0 / readingPpi.load());
}
// The margin control counts 300 ppi pixels, from 0 to 120 in steps of 3.
inline int marginDesign(int pixels) {
    return qRound(pixels * 300.0 / readingPpi.load());
}
}
