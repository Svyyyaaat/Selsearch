// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>

namespace selsearch {

class Config;

// The accent colour, in order of preference:
//  1. general.accent (an explicit "#rrggbb"),
//  2. the xdg-desktop-portal accent (only if general.follow_portal_accent),
//  3. Selsearch's built-in colour.
// The portal is only asked once per process and only when enabled; desktops
// without it (most wlroots setups) simply fall through to 3.
QColor accentColor(const Config &config);

QColor defaultAccent();

}  // namespace selsearch
