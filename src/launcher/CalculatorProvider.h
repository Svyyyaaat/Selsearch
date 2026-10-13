// SPDX-License-Identifier: MIT
#pragma once

#include "Provider.h"

namespace selsearch {

// Inline calculator and unit converter (see Calculator.h). Produces at most
// one row; activating it copies the full-precision value to the clipboard.
class CalculatorProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("calculator"); }
    QList<LauncherItem> query(const QString &text, int limit) override;
    bool activate(const LauncherItem &item) override;
};

}  // namespace selsearch
