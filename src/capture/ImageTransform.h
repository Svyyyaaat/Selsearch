// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QTransform>

namespace selsearch {

// Turn a buffer captured in an output's native orientation into the upright
// image the user sees. `transform` is a wl_output_transform value.
// Always returns a deep copy (the input may point into an mmap'ed buffer).
inline QImage applyOutputTransform(const QImage &buffer, int transform, bool yInvert)
{
    QImage img = buffer.copy();
    if (yInvert)
        img = img.mirrored(false, true);
    const bool flipped = transform >= 4;  // WL_OUTPUT_TRANSFORM_FLIPPED*
    const int rotation = (transform & 3) * 90;
    if (rotation != 0) {
        // The compositor rotated the scene counter-clockwise into the buffer;
        // rotating the buffer clockwise by the same angle restores it.
        img = img.transformed(QTransform().rotate(rotation));
    }
    if (flipped)
        img = img.mirrored(true, false);
    return img;
}

}  // namespace selsearch
