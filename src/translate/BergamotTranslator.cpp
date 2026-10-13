// SPDX-License-Identifier: MIT
#include "LanguageDetect.h"
#include "TranslationLanguages.h"
#include "TranslatorBackends.h"

#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace selsearch {

namespace {

QStringList codesFor(const QString &raw)
{
    const QString c = raw.trimmed().toLower();
    // Serbo-Croatian models translate Serbian, Croatian and Bosnian.
    if (c == QLatin1String("hbs") || c == QLatin1String("sh"))
        return {QStringLiteral("sr"), QStringLiteral("hr"), QStringLiteral("bs")};
    const QString n = normalizeLanguageCode(c);
    return n.isEmpty() ? QStringList{} : QStringList{n};
}

int typeRank(const QString &type)
{
    // base models are noticeably better and still fast on a CPU.
    if (type == QLatin1String("base"))
        return 3;
    if (type.startsWith(QLatin1String("base")))
        return 2;
    if (type.startsWith(QLatin1String("tiny")))
        return 1;
    return 0;
}

// Splits text on any kind of line break.
QStringList splitLines(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("\\r\\n|[\\r\\n\\x{2028}\\x{2029}\\x{0085}]"));
    return text.split(re);
}

void deliver(QObject *context, Translator::Callback cb, TranslationResult result)
{
    QTimer::singleShot(0, context, [cb = std::move(cb), result = std::move(result)] { cb(result); });
}

}  // namespace

BergamotTranslator::BergamotTranslator(QObject *parent, const QString &program)
    : Translator(parent)
    , m_program(program)
{
}

QString BergamotTranslator::executable() const
{
    if (m_program.contains(QLatin1Char('/')))
        return QFileInfo::exists(m_program) ? m_program : QString();
    return QStandardPaths::findExecutable(m_program);
}

bool BergamotTranslator::isAvailable() const
{
    if (executable().isEmpty())
        return false;
    return !m_modelsLoaded || !m_models.isEmpty();
}

QList<BergamotTranslator::Model> BergamotTranslator::parseModelList(const QByteArray &output)
{
    static const QRegularExpression invokeRe(QStringLiteral("-m\\s+(\\S+)\\s*$"));
    static const QRegularExpression typeRe(QStringLiteral("\\btype:\\s*(\\S+)"));
    static const QRegularExpression versionRe(QStringLiteral("\\bversion:\\s*(\\d+)"));
    QList<Model> out;
    for (const QString &rawLine : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        const auto m = invokeRe.match(line);
        if (!m.hasMatch())
            continue;
        Model model;
        model.shortName = m.captured(1);
        const QStringList parts = model.shortName.split(QLatin1Char('-'));
        if (parts.size() < 2)
            continue;
        model.sources = codesFor(parts[0]);
        const QStringList targets = codesFor(parts[1]);
        if (model.sources.isEmpty() || targets.isEmpty())
            continue;
        model.target = targets.first();
        model.type = parts.mid(2).join(QLatin1Char('-'));
        if (const auto t = typeRe.match(line); t.hasMatch())
            model.type = t.captured(1);
        if (const auto v = versionRe.match(line); v.hasMatch())
            model.version = v.captured(1).toInt();
        out << model;
    }
    return out;
}

void BergamotTranslator::loadModels(std::function<void(bool)> done, bool reload)
{
    // An empty list is re-read: the user may have installed a model meanwhile.
    if (m_modelsLoaded && !reload && !m_loading && !m_models.isEmpty()) {
        QTimer::singleShot(0, this, [done = std::move(done)] { done(true); });
        return;
    }
    m_waiting << std::move(done);
    if (m_loading)
        return;

    auto finish = [this](bool ok) {
        m_loading = false;
        m_modelsLoaded = true;
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &cb : waiting)
            cb(ok && !m_models.isEmpty());
    };

    const QString exe = executable();
    if (exe.isEmpty()) {
        m_models.clear();
        m_loading = true;
        QTimer::singleShot(0, this, [finish] { finish(false); });
        return;
    }
    m_loading = true;
    auto *proc = new QProcess(this);
    proc->setProgram(exe);
    proc->setArguments({QStringLiteral("-l")});
    proc->setStandardInputFile(QProcess::nullDevice());
    auto *timer = new QTimer(proc);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, proc, [proc] { proc->kill(); });
    connect(proc, &QProcess::errorOccurred, this, [proc, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            proc->deleteLater();
            finish(false);
        }
    });
    connect(proc, &QProcess::finished, this, [this, proc, finish](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        const bool ok = status == QProcess::NormalExit && code == 0;
        m_models = ok ? parseModelList(proc->readAllStandardOutput()) : QList<Model>{};
        finish(ok);
    });
    timer->start(15000);
    proc->start();
}

QStringList BergamotTranslator::route(const QString &source, const QString &target) const
{
    const QString s = normalizeLanguageCode(source);
    const QString t = normalizeLanguageCode(target);
    if (s.isEmpty() || t.isEmpty() || s == t)
        return {};
    auto pick = [this](const QString &from, const QString &to) -> QString {
        const Model *best = nullptr;
        for (const Model &m : m_models) {
            if (m.target != to || !m.sources.contains(from))
                continue;
            if (!best || typeRank(m.type) > typeRank(best->type)
                || (typeRank(m.type) == typeRank(best->type) && m.version > best->version))
                best = &m;
        }
        return best ? best->shortName : QString();
    };
    const QString direct = pick(s, t);
    if (!direct.isEmpty())
        return {direct};
    const QString en = QStringLiteral("en");
    if (s == en || t == en)
        return {};
    const QString first = pick(s, en);
    const QString second = pick(en, t);
    if (first.isEmpty() || second.isEmpty())
        return {};
    return {first, second};
}

void BergamotTranslator::translate(const TranslationRequest &request, Callback callback)
{
    QString source = normalizeLanguageCode(request.source);
    QString target = normalizeLanguageCode(request.target);
    if (target.isEmpty())
        target = systemLanguageCode();
    QString detected;
    if (source.isEmpty()) {
        detected = detectLanguage(request.segments).code;
        source = detected;
    }

    TranslationResult base;
    base.backend = id();
    base.detectedSource = source;

    if (source.isEmpty() || source == target) {
        base.segments = request.segments;
        deliver(this, std::move(callback), base);
        return;
    }

    // One input line per translateLocally line. Line breaks inside a segment
    // become separate lines and are put back afterwards; blank lines are not
    // sent at all.
    struct Piece {
        int line = -1;  // index into `lines`, -1 = keep `text` unchanged
        QString text;
    };
    auto plan = std::make_shared<QList<QList<Piece>>>();
    QStringList lines;
    for (const QString &segment : request.segments) {
        QList<Piece> pieces;
        for (const QString &part : splitLines(segment)) {
            QString clean = part;
            clean.replace(QLatin1Char('\t'), QLatin1Char(' '));
            clean = clean.trimmed();
            if (clean.isEmpty()) {
                pieces.append({-1, part});
            } else {
                pieces.append({int(lines.size()), clean});
                lines << clean;
            }
        }
        plan->append(pieces);
    }
    if (lines.isEmpty()) {
        base.segments = request.segments;
        deliver(this, std::move(callback), base);
        return;
    }

    loadModels([this, source, target, lines, plan, base, callback = std::move(callback)](bool) mutable {
        if (executable().isEmpty()) {
            base.error = tr("translateLocally is not installed.");
            callback(base);
            return;
        }
        const QStringList chain = route(source, target);
        if (chain.isEmpty()) {
            base.error = tr("No translateLocally model for %1 → %2. Download one with "
                            "`translateLocally -a` / `translateLocally -d <model>`.")
                             .arg(languageEnglishName(source), languageEnglishName(target));
            callback(base);
            return;
        }
        runChain(chain, lines, [plan, base, callback](QStringList out, QString error) mutable {
            if (!error.isEmpty()) {
                base.error = error;
                callback(base);
                return;
            }
            for (const QList<Piece> &pieces : *plan) {
                QStringList parts;
                for (const Piece &p : pieces)
                    parts << (p.line >= 0 ? out.value(p.line) : p.text);
                base.segments << parts.join(QLatin1Char('\n'));
            }
            callback(base);
        });
    });
}

void BergamotTranslator::runChain(QStringList chain, QStringList lines,
                                  std::function<void(QStringList, QString)> done)
{
    if (chain.isEmpty()) {
        done(lines, {});
        return;
    }
    const QString model = chain.takeFirst();
    QPointer<BergamotTranslator> self(this);
    runModel(model, lines, [self, chain, done](QStringList out, QString error) {
        if (!error.isEmpty() || !self) {
            done({}, error.isEmpty() ? QStringLiteral("cancelled") : error);
            return;
        }
        self->runChain(chain, out, done);
    });
}

void BergamotTranslator::runModel(const QString &model, const QStringList &lines,
                                  std::function<void(QStringList, QString)> done)
{
    auto *proc = new QProcess(this);
    proc->setProgram(executable());
    proc->setArguments({QStringLiteral("-m"), model});
    auto finished = std::make_shared<bool>(false);
    auto complete = [proc, finished, done](QStringList out, QString error) {
        if (*finished)
            return;
        *finished = true;
        proc->deleteLater();
        done(std::move(out), std::move(error));
    };

    auto *timer = new QTimer(proc);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, proc, [proc, complete, model, ms = m_timeoutMs] {
        proc->disconnect();
        proc->kill();
        complete({}, QObject::tr("translateLocally (%1) timed out after %2 s.").arg(model).arg(ms / 1000));
    });
    connect(proc, &QProcess::errorOccurred, proc, [proc, complete](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            complete({}, QObject::tr("Could not start translateLocally: %1").arg(proc->errorString()));
    });
    connect(proc, &QProcess::finished, proc, [proc, complete, model, expected = lines.size()](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0) {
            QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
            if (err.isEmpty())
                err = status == QProcess::NormalExit ? QStringLiteral("exit code %1").arg(code)
                                                      : QStringLiteral("crashed");
            complete({}, QObject::tr("translateLocally -m %1 failed: %2").arg(model, err));
            return;
        }
        QStringList out = QString::fromUtf8(proc->readAllStandardOutput()).split(QLatin1Char('\n'));
        for (QString &l : out)
            if (l.endsWith(QLatin1Char('\r')))
                l.chop(1);
        while (out.size() > expected && out.last().trimmed().isEmpty())
            out.removeLast();
        if (out.size() != expected) {
            complete({}, QObject::tr("translateLocally -m %1 returned %2 lines for %3 input lines.")
                             .arg(model)
                             .arg(out.size())
                             .arg(expected));
            return;
        }
        for (QString &l : out)
            l = l.trimmed();
        complete(out, {});
    });

    QByteArray input = lines.join(QLatin1Char('\n')).toUtf8();
    input.append('\n');
    timer->start(m_timeoutMs);
    proc->start();
    proc->write(input);
    proc->closeWriteChannel();
}

}  // namespace selsearch
