// SPDX-License-Identifier: MIT
.pragma library

// WCAG relative luminance of a QML color (0 black .. 1 white).
function luminance(c) {
    function lin(v) { return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
}
