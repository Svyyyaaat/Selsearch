// SPDX-License-Identifier: MIT
#include "LanguageDetect.h"
#include "TranslationLanguages.h"
#include "TranslatorBackends.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

namespace selsearch {

namespace {

QString stripThinking(QString s)
{
    static const QRegularExpression think(QStringLiteral("<think>.*?</think>"),
                                          QRegularExpression::DotMatchesEverythingOption);
    s.remove(think);
    // An unterminated <think> (cut off) hides everything after it.
    const qsizetype open = s.indexOf(QLatin1String("<think>"));
    if (open >= 0)
        s.truncate(open);
    return s;
}

QString stripFence(const QString &s)
{
    static const QRegularExpression fence(QStringLiteral("```[A-Za-z0-9_-]*[ \\t]*\\r?\\n?(.*?)```"),
                                          QRegularExpression::DotMatchesEverythingOption);
    const auto m = fence.match(s);
    if (m.hasMatch())
        return m.captured(1);
    // Unterminated fence.
    const qsizetype open = s.indexOf(QLatin1String("```"));
    if (open >= 0) {
        const qsizetype nl = s.indexOf(QLatin1Char('\n'), open);
        return nl >= 0 ? s.mid(nl + 1) : s.mid(open + 3);
    }
    return s;
}

std::optional<QStringList> fromArray(const QJsonArray &arr)
{
    QStringList out;
    for (const QJsonValue &v : arr) {
        if (v.isString()) {
            out << v.toString();
        } else if (v.isDouble()) {
            out << QString::number(v.toDouble());
        } else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            QString text;
            for (const auto *key : {"translation", "text", "target", "translated"}) {
                text = o.value(QLatin1String(key)).toString();
                if (!text.isEmpty())
                    break;
            }
            if (text.isEmpty()) {
                for (auto it = o.begin(); it != o.end() && text.isEmpty(); ++it)
                    if (it.value().isString())
                        text = it.value().toString();
            }
            out << text;
        } else {
            return std::nullopt;
        }
    }
    return out;
}

QList<qsizetype> positions(const QString &s, QChar c, bool fromEnd, int limit)
{
    QList<qsizetype> out;
    if (fromEnd) {
        for (qsizetype i = s.size() - 1; i >= 0 && out.size() < limit; --i)
            if (s.at(i) == c)
                out << i;
    } else {
        for (qsizetype i = 0; i < s.size() && out.size() < limit; ++i)
            if (s.at(i) == c)
                out << i;
    }
    return out;
}

QString unquote(QString line)
{
    line = line.trimmed();
    if (line.endsWith(QLatin1Char(',')))
        line.chop(1);
    line = line.trimmed();
    if (line.size() >= 2 && line.startsWith(QLatin1Char('"')) && line.endsWith(QLatin1Char('"'))) {
        const QJsonArray a = QJsonDocument::fromJson("[" + line.toUtf8() + "]").array();
        if (a.size() == 1 && a.first().isString())
            return a.first().toString();
        return line.mid(1, line.size() - 2);
    }
    return line;
}

}  // namespace

LlmTranslator::LlmTranslator(const AiSettings &settings, QObject *parent)
    : Translator(parent)
    , m_settings(settings)
    , m_client(new AiClient(this))
{
    m_settings.temperature = 0.1;
    m_client->setSettings(m_settings);
    connect(m_client, &AiClient::finished, this, &LlmTranslator::onAnswer);
    connect(m_client, &AiClient::failed, this, &LlmTranslator::onError);
}

QList<QList<int>> LlmTranslator::makeBatches(const QStringList &segments, int maxSegments, int maxChars)
{
    QList<QList<int>> batches;
    QList<int> cur;
    int chars = 0;
    for (int i = 0; i < segments.size(); ++i) {
        if (segments[i].trimmed().isEmpty())
            continue;
        const int len = int(segments[i].size());
        if (!cur.isEmpty() && (cur.size() >= maxSegments || chars + len > maxChars)) {
            batches << cur;
            cur.clear();
            chars = 0;
        }
        cur << i;
        chars += len;
    }
    if (!cur.isEmpty())
        batches << cur;
    return batches;
}

QString LlmTranslator::systemPrompt(const QString &source, const QString &target, int count)
{
    const QString from = source.isEmpty() ? QStringLiteral("the source language (detect it)")
                                          : languageEnglishName(source);
    const QString to = languageEnglishName(target);
    return QStringLiteral(
               "You are a translation engine. The user sends a JSON array of %1 strings. Translate each string "
               "from %2 into %3.\n"
               "Rules:\n"
               "- Reply with ONLY a JSON array of exactly %1 strings: the translations, in the same order.\n"
               "- Never merge, split, skip or reorder items. No explanations, notes, transliterations or "
               "markdown.\n"
               "- Keep numbers, URLs, e-mail addresses, code, placeholders and line breaks unchanged.\n"
               "- If an item is already in %3, or cannot be translated, copy it unchanged.")
        .arg(count)
        .arg(from, to);
}

std::optional<QStringList> LlmTranslator::parseReply(const QString &reply, int expected)
{
    const QString noThink = stripThinking(reply);
    const QString body = stripFence(noThink);

    for (const QString &text : {body, noThink}) {
        // A JSON array somewhere in the text.
        for (qsizetype start : positions(text, QLatin1Char('['), false, 6)) {
            for (qsizetype end : positions(text, QLatin1Char(']'), true, 6)) {
                if (end <= start)
                    break;
                QJsonParseError err;
                const QJsonDocument doc = QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8(), &err);
                if (err.error != QJsonParseError::NoError || !doc.isArray())
                    continue;
                if (auto list = fromArray(doc.array()); list && list->size() == expected)
                    return list;
            }
        }
        // {"translations": [...]}
        const qsizetype open = text.indexOf(QLatin1Char('{'));
        const qsizetype close = text.lastIndexOf(QLatin1Char('}'));
        if (open >= 0 && close > open) {
            const QJsonDocument doc = QJsonDocument::fromJson(text.mid(open, close - open + 1).toUtf8());
            const QJsonObject o = doc.object();
            for (auto it = o.begin(); it != o.end(); ++it) {
                if (!it.value().isArray())
                    continue;
                if (auto list = fromArray(it.value().toArray()); list && list->size() == expected)
                    return list;
            }
        }
    }

    // One line per item, possibly numbered or bulleted.
    static const QRegularExpression bullet(QStringLiteral("^\\s*(?:\\d+[.):]|[-*•])\\s+"));
    QStringList lines;
    for (QString line : body.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line == QLatin1String("[") || line == QLatin1String("]"))
            continue;
        line.remove(bullet);
        lines << unquote(line);
    }
    if (lines.size() == expected)
        return lines;
    if (expected == 1) {
        const QString whole = unquote(body.trimmed());
        if (!whole.isEmpty())
            return QStringList{whole};
    }
    return std::nullopt;
}

void LlmTranslator::translate(const TranslationRequest &request, Callback callback)
{
    auto job = std::make_shared<Job>();
    job->segments = request.segments;
    job->out = request.segments;
    job->source = normalizeLanguageCode(request.source);
    job->target = normalizeLanguageCode(request.target);
    if (job->target.isEmpty())
        job->target = systemLanguageCode();
    job->callback = std::move(callback);
    if (job->source.isEmpty()) {
        const DetectedLanguage d = detectLanguage(request.segments);
        job->detected = d.code;
        if (d.confidence >= 0.5)
            job->source = d.code;
    } else {
        job->detected = job->source;
    }
    if (job->source.isEmpty() || job->source != job->target)
        job->pending = makeBatches(request.segments);
    m_jobs << job;
    QTimer::singleShot(0, this, &LlmTranslator::pump);
}

void LlmTranslator::pump()
{
    while (!m_busy && !m_jobs.isEmpty()) {
        const auto job = m_jobs.first();
        if (job->pending.isEmpty()) {
            finishJob({});
            continue;
        }
        m_batch = job->pending.takeFirst();
        QJsonArray items;
        for (int i : m_batch)
            items.append(job->segments[i]);
        QList<AiMessage> messages;
        messages.append({AiMessage::System, systemPrompt(job->source, job->target, int(m_batch.size())), {}});
        messages.append({AiMessage::User, QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact)), {}});
        m_busy = true;
        m_client->ask(messages);
    }
}

void LlmTranslator::onAnswer(const QString &text)
{
    m_busy = false;
    if (m_jobs.isEmpty())
        return;
    const auto job = m_jobs.first();
    if (const auto parsed = parseReply(text, int(m_batch.size()))) {
        for (int k = 0; k < m_batch.size(); ++k)
            job->out[m_batch[k]] = parsed->at(k);
    } else if (m_batch.size() > 1) {
        // The model lost count: retry in two smaller halves.
        const qsizetype half = m_batch.size() / 2;
        job->pending.prepend(m_batch.mid(half));
        job->pending.prepend(m_batch.mid(0, half));
    } else {
        finishJob(tr("The AI model returned no usable translation."));
    }
    m_batch.clear();
    pump();
}

void LlmTranslator::onError(const QString &error)
{
    m_busy = false;
    m_batch.clear();
    if (!m_jobs.isEmpty())
        finishJob(error);
    pump();
}

void LlmTranslator::finishJob(const QString &error)
{
    const auto job = m_jobs.takeFirst();
    TranslationResult r;
    r.backend = id();
    r.detectedSource = job->source.isEmpty() ? job->detected : job->source;
    if (error.isEmpty())
        r.segments = job->out;
    else
        r.error = error;
    job->callback(r);
}

}  // namespace selsearch
