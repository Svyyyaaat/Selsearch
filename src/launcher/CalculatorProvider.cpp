// SPDX-License-Identifier: MIT
#include "CalculatorProvider.h"

#include "Calculator.h"

#include <QClipboard>
#include <QGuiApplication>

namespace selsearch {

QList<LauncherItem> CalculatorProvider::query(const QString &text, int limit)
{
    if (limit <= 0)
        return {};
    const auto r = calc::evaluate(text);
    if (!r)
        return {};
    LauncherItem item;
    item.kind = LauncherItem::Calculator;
    item.id = QStringLiteral("calculator");
    item.title = r->title;
    item.subtitle = QStringLiteral("%1 = %2").arg(text.trimmed(), r->grouped);
    item.icon = QStringLiteral("lucide:calculator");
    item.group = QStringLiteral("Calculator");
    item.score = 1000;
    item.data.insert(QStringLiteral("value"), r->copyText);
    item.data.insert(QStringLiteral("unit"), r->unit);
    return {item};
}

bool CalculatorProvider::activate(const LauncherItem &item)
{
    const QString value = item.data.value(QStringLiteral("value")).toString();
    if (value.isEmpty() || !qGuiApp)
        return false;
    // Clipboard ownership on Wayland lives with the daemon, which stays alive.
    QGuiApplication::clipboard()->setText(value);
    return true;
}

}  // namespace selsearch
