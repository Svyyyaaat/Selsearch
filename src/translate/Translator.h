// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace selsearch {

class Config;

struct TranslationRequest {
    QStringList segments;  // translated independently, order preserved
    QString source;        // ISO 639-1 code, or empty / "auto" to detect
    QString target;        // ISO 639-1 code
};

struct TranslationResult {
    QStringList segments;  // same length as the request on success
    QString detectedSource;
    QString backend;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Asynchronous translator. translate() is called on the GUI thread and the
// callback is invoked on the GUI thread.
class Translator : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const TranslationResult &)>;
    using QObject::QObject;
    virtual QString id() const = 0;
    virtual bool isAvailable() const = 0;
    virtual void translate(const TranslationRequest &request, Callback callback) = 0;
};

// Backends:
//  - "bergamot":      Firefox Translations models through the translateLocally CLI (local)
//  - "libretranslate": a LibreTranslate HTTP server (usually self-hosted)
//  - "llm":           any OpenAI-compatible chat endpoint (ai.* config)
// "auto" picks the first available in the order above (see TranslatorFactory.cpp).
// The unique_ptr owns the translator. If `parent` is given it must outlive the
// returned object (or release() it to hand ownership to the parent).
// The concrete backends are declared in TranslatorBackends.h.
std::unique_ptr<Translator> createTranslator(const Config &config, QObject *parent = nullptr);

}  // namespace selsearch
