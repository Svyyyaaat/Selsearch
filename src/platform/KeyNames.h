// SPDX-License-Identifier: MIT
#pragma once

#include <QKeySequence>
#include <QString>

namespace selsearch {

// QKeySequence → the syntax of each place a shortcut can live.
QString toXdgTrigger(const QKeySequence &keys);   // portal: "LOGO+SHIFT+s"
QString toHyprlandKeys(const QKeySequence &keys); // hl.bind: "SUPER + SHIFT + S"
QString toSwayKeys(const QKeySequence &keys);     // bindsym: "Mod4+Shift+s"
// Human-readable, e.g. "Super + Shift + S".
QString toDisplayKeys(const QKeySequence &keys);

}  // namespace selsearch
