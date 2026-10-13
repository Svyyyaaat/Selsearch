// SPDX-License-Identifier: MIT
#include "LanguageDetect.h"
#include "TranslationLanguages.h"
#include "Translator.h"
#include "TranslatorBackends.h"

#include "core/Config.h"

#include <QPointer>
#include <QTimer>

namespace selsearch {

namespace {

constexpr int kLibreProbeTimeoutMs = 500;

std::unique_ptr<LlmTranslator> makeLlm(const Config &config, QObject *parent)
{
    if (!config.boolean(QStringLiteral("ai.enabled")))
        return nullptr;
    return std::make_unique<LlmTranslator>(aiSettingsFromConfig(config), parent);
}

// "auto": the first backend that can do the job, in order
//   bergamot (a model route exists for the pair),
//   libretranslate (the server answered /languages within 500 ms; cached),
//   llm (ai.enabled).
// When a backend fails at run time the next one is tried; the first error is
// reported if all fail.
class AutoTranslator : public Translator {
public:
    AutoTranslator(const Config &config, QObject *parent)
        : Translator(parent)
    {
        m_bergamot = new BergamotTranslator(this);
        const QString url = config.string(QStringLiteral("translate.libretranslate_url")).trimmed();
        if (!url.isEmpty())
            m_libre = new LibreTranslateTranslator(url, config.string(QStringLiteral("translate.libretranslate_key")), this);
        if (auto llm = makeLlm(config, this))
            m_llm = llm.release();
    }

    QString id() const override { return QStringLiteral("auto"); }

    bool isAvailable() const override
    {
        return m_bergamot->isAvailable() || (m_libre && m_libre->isAvailable()) || (m_llm && m_llm->isAvailable());
    }

    void translate(const TranslationRequest &request, Callback callback) override
    {
        auto state = std::make_shared<State>();
        state->request = request;
        state->callback = std::move(callback);
        state->source = normalizeLanguageCode(request.source);
        state->target = normalizeLanguageCode(request.target);
        if (state->target.isEmpty())
            state->target = systemLanguageCode();
        state->request.target = state->target;
        if (state->source.isEmpty())
            state->detected = detectLanguage(request.segments).code;
        step(state, 0);
    }

private:
    struct State {
        TranslationRequest request;
        Callback callback;
        QString source;    // as requested (empty = auto)
        QString target;
        QString detected;  // local guess when source is empty
        QString firstError;
    };

    void step(const std::shared_ptr<State> &state, int stage)
    {
        QPointer<AutoTranslator> self(this);
        auto next = [self, state, stage] {
            if (self)
                self->step(state, stage + 1);
        };
        auto run = [self, state, next](Translator *backend, const TranslationRequest &req) {
            backend->translate(req, [self, state, next](const TranslationResult &r) {
                if (r.ok() || !self) {
                    state->callback(r);
                    return;
                }
                if (state->firstError.isEmpty())
                    state->firstError = r.error;
                next();
            });
        };

        switch (stage) {
        case 0: {  // bergamot
            if (m_bergamot->executable().isEmpty())
                return next();
            const QString src = state->source.isEmpty() ? state->detected : state->source;
            m_bergamot->loadModels([self, state, src, next, run](bool) {
                if (!self)
                    return;
                if (src.isEmpty() || !self->m_bergamot->canTranslate(src, state->target)) {
                    next();
                    return;
                }
                TranslationRequest req = state->request;
                req.source = src;
                run(self->m_bergamot, req);
            });
            return;
        }
        case 1:  // libretranslate
            if (!m_libre)
                return next();
            m_libre->probe(kLibreProbeTimeoutMs, [self, state, next, run](bool ok) {
                if (!self)
                    return;
                if (!ok)
                    return next();
                run(self->m_libre, state->request);
            });
            return;
        case 2:  // llm
            if (!m_llm || !m_llm->isAvailable())
                return next();
            run(m_llm, state->request);
            return;
        default: {
            TranslationResult r;
            r.backend = id();
            r.detectedSource = state->source.isEmpty() ? state->detected : state->source;
            r.error = !state->firstError.isEmpty()
                ? state->firstError
                : tr("No translation backend is available. Install translateLocally with a model for "
                     "%1 → %2, run a LibreTranslate server, or enable AI.")
                      .arg(languageEnglishName(r.detectedSource.isEmpty() ? QStringLiteral("auto") : r.detectedSource),
                           languageEnglishName(state->target));
            state->callback(r);
        }
        }
    }

    BergamotTranslator *m_bergamot = nullptr;
    LibreTranslateTranslator *m_libre = nullptr;
    LlmTranslator *m_llm = nullptr;
};

}  // namespace

std::unique_ptr<Translator> createTranslator(const Config &config, QObject *parent)
{
    const QString backend = config.string(QStringLiteral("translate.backend")).trimmed().toLower();
    if (backend == QLatin1String("bergamot") || backend == QLatin1String("translatelocally"))
        return std::make_unique<BergamotTranslator>(parent);
    if (backend == QLatin1String("libretranslate"))
        return std::make_unique<LibreTranslateTranslator>(config.string(QStringLiteral("translate.libretranslate_url")),
                                                          config.string(QStringLiteral("translate.libretranslate_key")),
                                                          parent);
    if (backend == QLatin1String("llm")) {
        // Explicitly chosen: use the AI endpoint even if the assistant UI is off.
        return std::make_unique<LlmTranslator>(aiSettingsFromConfig(config), parent);
    }
    return std::make_unique<AutoTranslator>(config, parent);
}

}  // namespace selsearch
