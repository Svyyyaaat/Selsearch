// SPDX-License-Identifier: MIT
#include "ModelStore.h"

#include "OcrService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <tuple>

namespace selsearch {

namespace {

// Official PaddlePaddle ONNX exports of the PP-OCRv5 mobile models, Apache-2.0.
// URLs are pinned to a repository commit so the content cannot change under the
// hash. `inference.yml` carries the recognition dictionary (PostProcess /
// character_dict); the ONNX files have no embedded "character" metadata.
const QString kHf = QStringLiteral("https://huggingface.co/PaddlePaddle/%1_onnx/resolve/%2/%3");
const QString kLicense = QStringLiteral("Apache-2.0");

ModelSpec rec(const char *id, const char *commit, qint64 onnxSize, const char *onnxSha, qint64 ymlSize,
              const char *ymlSha, QStringList langs)
{
    const QString sid = QString::fromLatin1(id);
    const QString c = QString::fromLatin1(commit);
    ModelSpec s;
    s.id = sid;
    s.kind = ModelSpec::Recognition;
    s.files = {
        {sid + QStringLiteral(".onnx"), kHf.arg(sid, c, QStringLiteral("inference.onnx")), QByteArray(onnxSha),
         onnxSize},
        {sid + QStringLiteral(".yml"), kHf.arg(sid, c, QStringLiteral("inference.yml")), QByteArray(ymlSha), ymlSize},
    };
    s.languages = std::move(langs);
    s.license = kLicense;
    return s;
}

QStringList L(std::initializer_list<const char *> codes)
{
    QStringList out;
    for (const char *c : codes)
        out << QString::fromLatin1(c);
    return out;
}

QList<ModelSpec> buildDefaultSpecs()
{
    QList<ModelSpec> specs;
    {
        ModelSpec det;
        det.id = QStringLiteral("PP-OCRv5_mobile_det");
        det.kind = ModelSpec::Detection;
        det.files = {{QStringLiteral("PP-OCRv5_mobile_det.onnx"),
                      kHf.arg(det.id, QStringLiteral("e6f4fa85f00e168c862bc462aebca69eef9b3d3d"),
                              QStringLiteral("inference.onnx")),
                      "a431985659dc921974177a95adcfbb90fd9e51989a5e04d70d0b75f597b6e61d", 4826518}};
        det.license = kLicense;
        specs << det;
    }
    // Every recognition dictionary below includes ASCII letters, so every model
    // also reads English; "en" is listed for each of them.
    //
    // Table order matters: it is the final tie-breaker in bestRecognitionModel(),
    // so the general zh/en/ja model comes first and eslav precedes cyrillic.
    specs << rec("PP-OCRv5_mobile_rec", "ed152b8b495f84de93cda5709d768548a9127622",
                 16534782, "da72dc72ca4dc220df0dfde68c1dedc31c58d3e76a25871122e5056227d50092",
                 148345, "5dfeb2777f6d0db8177d8128a8acfcf6e6276dc4ac73ea3bf0dc06d6a5e85d8e",
                 L({"en", "zh", "ja"}));
    specs << rec("eslav_PP-OCRv5_mobile_rec", "9a32171fc5718746875e1a261818884517975013",
                 7887627, "b3018ef2b09a0250b6e0c8e871c927098363e5fd4df890cc68e8358eb0aaf1bd",
                 4538, "025039bac23eb4a308efcefa4d58eab3af440767815c6ba6938468bf6353ee5a",
                 L({"ru", "uk", "be", "en"}));
    specs << rec("latin_PP-OCRv5_mobile_rec", "89d3a50e2c27e2e7cceeab0e944c25c807d5db4f",
                 8042023, "7888113072263cb471b93f66dd5e2ad70548dc526fa1ace760d0d973dd121498",
                 6817, "0bbe984570f597af3638e50bdf2e8276f3ab26a61966096538b3b0d1849f5c84",
                 L({"en", "fr", "de", "es", "it", "pt", "nl", "pl", "cs", "sk", "sl", "hr", "bs", "ro", "hu", "fi",
                    "sv", "da", "no", "nb", "nn", "et", "lv", "lt", "tr", "id", "ms", "ca", "eu", "gl", "ga", "cy",
                    "is", "sq", "af", "sw", "tl", "la", "mt", "lb", "oc", "br", "rm", "fo", "az", "uz", "mi"}));
    specs << rec("cyrillic_PP-OCRv5_mobile_rec", "2cef88145434beb8afa9dd82d77d799eb1ad7b29",
                 8048799, "5371ee1ddaa7983cc62d0818d99e982b6804638c85e4f960d59a574094e172e5",
                 6991, "5c76cc91fa98410178a09f498db10050d0ec1634a660053d3005ab7be581f501",
                 L({"bg", "sr", "mk", "kk", "ky", "mn", "tg", "tt", "ba", "cv", "ce", "av", "os", "ab", "kv",
                    "ru", "uk", "be", "en"}));
    specs << rec("korean_PP-OCRv5_mobile_rec", "5c6f574b8e2230adf4287b33e736d71b9fabd28e",
                 13418787, "92f0b7785e64fc9090106a241cf4c1eb97472824558272751b88a2a4476d3a08",
                 96039, "f757fa1c40e99edcf27e9cce879b93eb2a51fa46f5ef39095689b8c37dd75998",
                 L({"ko", "en"}));
    specs << rec("arabic_PP-OCRv5_mobile_rec", "14aaedcd75825982689ecf5cd64ab33ee083215a",
                 7998947, "799113ebf267fbe742deb99eb36e8d42c9ddc5291ceacf92add41b4d52a59110",
                 6165, "21368419e6c016c31db55d316d59e11c128e1913e6e6fe10287084710043d3a6",
                 L({"ar", "fa", "ur", "ug", "ps", "sd", "en"}));
    specs << rec("devanagari_PP-OCRv5_mobile_rec", "251aec19e36739540d35e2cc943f6aa7503b98e5",
                 7912311, "cb789212ce96c69d3e74728ae4309d179281d68cb3945d0616b67cafab41c986",
                 5027, "9bd172dd26440c8ce94d1cde5d5baea6aefdc7cf3c5c8492e0beedef656d4e54",
                 L({"hi", "mr", "ne", "sa", "en"}));
    specs << rec("th_PP-OCRv5_mobile_rec", "1d4adbbafb1034a2fd6618498575b81ea7b69f69",
                 7891015, "27618be66018f8598ac0a526a593f9f1cebf794e7eded93428e8fb016e537f5f",
                 4667, "f6ba7fefc38ca1ff398ddafa75d67d16e0b3757c4e6c833adffee98a981766c9",
                 L({"th", "en"}));
    specs << rec("el_PP-OCRv5_mobile_rec", "8152b89d2ee0e1d4c92decab75fe75e5e3d836b6",
                 7808735, "2acf17fcaea2bc81b878e311e6263b8885f48bb03796f75f9f30ed3242bbaa6d",
                 3317, "17d85b2fe2d2f24cd4ab07bcbc33e0c126859b956ced36e281dc65e2d0c1f0bf",
                 L({"el", "en"}));
    specs << rec("ta_PP-OCRv5_mobile_rec", "0db89798af0d5218fad9ea7ace0557526e9077f1",
                 7885691, "c6d2b682d2a0ea4cb1fccdba295976f93fd439964d16cdc666cadef531accbee",
                 4579, "88a28f5a1bb30cabe38a0985cb5e6619fa4f0c7c78e57a08274674228c5219a6",
                 L({"ta", "en"}));
    specs << rec("te_PP-OCRv5_mobile_rec", "f1197c83430241c58c64b0952b3a80366d04eeba",
                 7898759, "8238bfc46d4cffe720ed6706e3842802467343497428693ff2bfb4e6b3caa36b",
                 4795, "acebbe53f1831bf28ddfed75aedf58225d7aa5d09100c1d5a9140a2a53b137ce",
                 L({"te", "en"}));
    return specs;
}

const ModelSpec *findIn(const QList<ModelSpec> &specs, const QString &id)
{
    for (const ModelSpec &s : specs)
        if (s.id == id)
            return &s;
    return nullptr;
}

// Recognition model choice.
//
// Each PP-OCRv5 recognizer reads one script family (plus ASCII/English), so we
// pick exactly one model per request:
//   1. Maximise the number of requested *non-English* languages it covers.
//      English is readable by every model, so it never decides on its own:
//      ru+en picks eslav, de+en picks latin, plain en picks the general model.
//   2. Then prefer covering English when English was requested (only matters
//      for hypothetical models without Latin letters).
//   3. Then prefer the model covering the language listed earliest in the
//      request (the user's primary language): ja+ru picks the general model,
//      ru+ja picks eslav.
//   4. Finally the table order: the general zh/en/ja model, then eslav before
//      the broader cyrillic model, etc.
// An empty or entirely unknown request yields the general model.
QString rank(const QList<ModelSpec> &specs, const QStringList &languagesIn, const QStringList &candidates)
{
    QStringList languages;
    for (const QString &l : languagesIn) {
        // Accept "pt_BR" / "zh-TW" style codes by keeping the primary subtag.
        const QString code = l.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0).toLower();
        if (!code.isEmpty() && !languages.contains(code))
            languages << code;
    }
    const QString en = QStringLiteral("en");
    QString best;
    std::tuple<int, int, int, int> bestKey{-1, -1, -1, -1};
    for (int i = 0; i < specs.size(); ++i) {
        const ModelSpec &s = specs[i];
        if (s.kind != ModelSpec::Recognition)
            continue;
        if (!candidates.isEmpty() && !candidates.contains(s.id))
            continue;
        int nonEn = 0;
        int firstIdx = int(languages.size());
        for (int j = 0; j < languages.size(); ++j) {
            if (!s.languages.contains(languages[j]))
                continue;
            if (languages[j] != en)
                ++nonEn;
            firstIdx = std::min(firstIdx, j);
        }
        const int coversEn = languages.contains(en) && s.languages.contains(en) ? 1 : 0;
        const std::tuple<int, int, int, int> key{nonEn, coversEn, -firstIdx, -i};
        if (best.isEmpty() || key > bestKey) {
            best = s.id;
            bestKey = key;
        }
    }
    return best;
}

QStringList normalizedLanguages(const QStringList &languagesIn)
{
    QStringList languages;
    for (const QString &l : languagesIn) {
        const QString code = l.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0).toLower();
        if (!code.isEmpty() && !languages.contains(code))
            languages << code;
    }
    return languages;
}

// Greedy cover in the user's order: for each requested non-English language
// not yet covered, add the best model (by rank()) among those that read it,
// judged against the languages still uncovered from there on.
QStringList cover(const QList<ModelSpec> &specs, const QStringList &languagesIn, const QStringList &candidates)
{
    const QStringList languages = normalizedLanguages(languagesIn);
    const QString en = QStringLiteral("en");
    QStringList chosen;
    auto covered = [&](const QString &lang) {
        for (const QString &id : std::as_const(chosen))
            if (const ModelSpec *s = findIn(specs, id); s && s->languages.contains(lang))
                return true;
        return false;
    };
    for (int i = 0; i < languages.size() && chosen.size() < ModelStore::kMaxRecognitionModels; ++i) {
        const QString &lang = languages[i];
        if (lang == en || covered(lang))
            continue;
        QStringList readers;
        for (const ModelSpec &s : specs)
            if (s.kind == ModelSpec::Recognition && s.languages.contains(lang)
                && (candidates.isEmpty() || candidates.contains(s.id)))
                readers << s.id;
        if (readers.isEmpty())
            continue;  // unknown language, or its model is not present
        QStringList rest;
        for (int j = i; j < languages.size(); ++j)
            if (!covered(languages[j]))
                rest << languages[j];
        chosen << rank(specs, rest, readers);
    }
    if (chosen.isEmpty()) {
        const QString fallback = rank(specs, languages, candidates);
        if (!fallback.isEmpty())
            chosen << fallback;
    }
    return chosen;
}

}  // namespace

struct ModelStore::Job {
    ModelFile file;
    QFile out;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 received = 0;
};

const QList<ModelSpec> &ModelStore::defaultSpecs()
{
    static const QList<ModelSpec> specs = buildDefaultSpecs();
    return specs;
}

QString ModelStore::detectionModelId()
{
    return QStringLiteral("PP-OCRv5_mobile_det");
}

const ModelSpec *ModelStore::findSpec(const QString &id)
{
    return findIn(defaultSpecs(), id);
}

QString ModelStore::bestRecognitionModel(const QStringList &languages, const QStringList &candidates)
{
    return rank(defaultSpecs(), languages, candidates);
}

QStringList ModelStore::recognitionModelsFor(const QStringList &languages, const QStringList &candidates)
{
    return cover(defaultSpecs(), languages, candidates);
}

bool ModelStore::isPresent(const QString &dir, const ModelSpec &spec)
{
    for (const ModelFile &f : spec.files) {
        const QFileInfo fi(dir + QLatin1Char('/') + f.fileName);
        if (!fi.isFile() || (f.size > 0 && fi.size() != f.size))
            return false;
    }
    return !spec.files.isEmpty();
}

QStringList ModelStore::presentRecognitionModels(const QString &dir)
{
    QStringList out;
    for (const ModelSpec &s : defaultSpecs())
        if (s.kind == ModelSpec::Recognition && isPresent(dir, s))
            out << s.id;
    return out;
}

ModelStore::ModelStore(QObject *parent)
    : ModelStore(OcrService::modelDir(), defaultSpecs(), parent)
{
}

ModelStore::ModelStore(const QString &dir, QList<ModelSpec> specs, QObject *parent)
    : QObject(parent)
    , m_dir(dir)
    , m_specs(std::move(specs))
{
}

ModelStore::~ModelStore()
{
    cancel();
}

const ModelSpec *ModelStore::spec(const QString &id) const
{
    return findIn(m_specs, id);
}

QStringList ModelStore::requiredFor(const QStringList &languages) const
{
    QStringList ids;
    for (const ModelSpec &s : m_specs)
        if (s.kind == ModelSpec::Detection) {
            ids << s.id;
            break;
        }
    ids << cover(m_specs, languages, {});
    return ids;
}

QStringList ModelStore::missingFor(const QStringList &languages) const
{
    QStringList out;
    for (const QString &id : requiredFor(languages))
        if (const ModelSpec *s = spec(id); s && !isPresent(m_dir, *s))
            out << id;
    return out;
}

void ModelStore::ensure(const QStringList &languages)
{
    if (isBusy()) {
        QTimer::singleShot(0, this, [this] { Q_EMIT failed(QStringLiteral("a model download is already running")); });
        return;
    }
    m_queue.clear();
    m_doneBytes = 0;
    m_totalBytes = 0;
    for (const QString &id : missingFor(languages)) {
        for (const ModelFile &f : spec(id)->files) {
            const QFileInfo fi(m_dir + QLatin1Char('/') + f.fileName);
            if (fi.isFile() && fi.size() == f.size)
                continue;
            m_queue << f;
            m_totalBytes += f.size;
        }
    }
    if (m_queue.isEmpty()) {
        QTimer::singleShot(0, this, [this] { Q_EMIT finished(); });
        return;
    }
    if (!QDir().mkpath(m_dir)) {
        m_queue.clear();
        QTimer::singleShot(0, this, [this] { Q_EMIT failed(QStringLiteral("cannot create %1").arg(m_dir)); });
        return;
    }
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QTimer::singleShot(0, this, &ModelStore::startNext);
}

void ModelStore::cancel()
{
    m_queue.clear();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    if (m_job) {
        m_job->out.remove();
        m_job.reset();
    }
}

void ModelStore::startNext()
{
    if (m_reply)
        return;
    if (m_queue.isEmpty()) {
        Q_EMIT finished();
        return;
    }
    m_job = std::make_unique<Job>();
    m_job->file = m_queue.takeFirst();
    m_job->out.setFileName(m_dir + QLatin1Char('/') + m_job->file.fileName + QStringLiteral(".part"));
    if (!m_job->out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("cannot write %1: %2").arg(m_job->out.fileName(), m_job->out.errorString()));
        return;
    }
    QNetworkRequest req{QUrl(m_job->file.url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("selsearch/" SELSEARCH_VERSION));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60'000);  // inactivity timeout
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelStore::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ModelStore::onFinished);
}

void ModelStore::onReadyRead()
{
    if (!m_reply || !m_job)
        return;
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty())
        return;
    m_job->received += chunk.size();
    if (m_job->file.size > 0 && m_job->received > m_job->file.size) {
        fail(QStringLiteral("%1: more data than expected").arg(m_job->file.fileName));
        return;
    }
    m_job->hash.addData(chunk);
    if (m_job->out.write(chunk) != chunk.size()) {
        fail(QStringLiteral("cannot write %1: %2").arg(m_job->out.fileName(), m_job->out.errorString()));
        return;
    }
    Q_EMIT progress(m_doneBytes + m_job->received, m_totalBytes);
}

void ModelStore::onFinished()
{
    if (!m_reply || !m_job)
        return;
    onReadyRead();
    if (!m_reply)  // failed while draining
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("download of %1 failed: %2").arg(m_job->file.fileName, reply->errorString()));
        return;
    }
    m_job->out.close();
    const QByteArray sha = m_job->hash.result().toHex();
    if (m_job->received != m_job->file.size || sha != m_job->file.sha256) {
        fail(QStringLiteral("%1: checksum mismatch (got %2 bytes, sha256 %3)")
                 .arg(m_job->file.fileName)
                 .arg(m_job->received)
                 .arg(QString::fromLatin1(sha)));
        return;
    }
    const QString final = m_dir + QLatin1Char('/') + m_job->file.fileName;
    QFile::remove(final);
    if (!m_job->out.rename(final)) {
        fail(QStringLiteral("cannot rename to %1: %2").arg(final, m_job->out.errorString()));
        return;
    }
    m_doneBytes += m_job->file.size;
    m_job.reset();
    startNext();
}

void ModelStore::fail(const QString &error)
{
    cancel();
    Q_EMIT failed(error);
}

}  // namespace selsearch
