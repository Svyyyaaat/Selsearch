// SPDX-License-Identifier: MIT
#pragma once

#include "Provider.h"

#include <QUrl>

namespace selsearch {

class Config;

// The "Web" group: an "Open <url>" row when the input looks like a URL or a
// domain, and always a final "Search <Engine> for “query”" row.
// Activation does not open anything itself: LauncherEngine hands the URL
// (item.data["url"]) to the daemon, which picks the results panel or the
// browser. activate() is a QDesktopServices fallback for other callers.
class WebProvider : public Provider {
public:
    explicit WebProvider(const Config *config = nullptr);

    QString id() const override { return QStringLiteral("web"); }
    QList<LauncherItem> query(const QString &text, int limit) override;
    bool activate(const LauncherItem &item) override;

    static QUrl urlFor(const LauncherItem &item);

    // A URL for inputs that clearly are one: an explicit scheme
    // ("https://x", "file:///", "mailto:a@b.c"), localhost[:port][/path], an
    // IPv4 address, or a host name with a plausible TLD ("example.com",
    // "sub.example.co.uk/path"). Schemeless inputs get https:// (http:// for
    // localhost and IPs). Invalid QUrl when it does not look like one; plain
    // words, numbers ("3.14") and file names ("notes.txt", "main.py") are
    // rejected.
    static QUrl detectUrl(const QString &text);

private:
    const Config *m_config;
};

}  // namespace selsearch
