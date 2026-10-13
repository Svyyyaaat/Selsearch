// SPDX-License-Identifier: MIT
#pragma once

#include "SearchEngines.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

namespace selsearch {

class Config;

// As-you-type search suggestions from the selected engine.
//
// PRIVACY: this sends what the user types (each debounced keystroke burst) to
// the search engine. It therefore only runs when search.suggestions is true,
// which is off by default. Requests carry no cookies (the network manager has
// a jar that never stores or sends any), a generic browser User-Agent, no
// Referer, and nothing else that identifies the user or the app.
class SuggestionFetcher : public QObject {
    Q_OBJECT
public:
    explicit SuggestionFetcher(const Config *config, QObject *parent = nullptr);
    ~SuggestionFetcher() override;

    // Schedule a fetch for `query` after the debounce delay; a newer call
    // replaces it, and an in-flight request for an older query is aborted.
    // Does nothing (and cancels) when suggestions are disabled, the query is
    // empty or the engine has no suggestion endpoint.
    void fetch(const QString &query);
    void cancel();

    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }
    // Use this engine instead of search.engine (tests, per-panel overrides).
    void setEngineOverride(const SearchEngine &engine) { m_override = engine; }
    bool isEnabled() const;

    static QByteArray userAgent();

Q_SIGNALS:
    void suggestionsReady(const QString &query, const QStringList &suggestions);
    void failed(const QString &query, const QString &error);

private:
    void start();
    SearchEngine engine() const;

    const Config *m_config;
    QNetworkAccessManager *m_nam;
    QTimer m_debounce;
    QString m_pending;
    QPointer<QNetworkReply> m_reply;
    SearchEngine m_override;
};

}  // namespace selsearch
