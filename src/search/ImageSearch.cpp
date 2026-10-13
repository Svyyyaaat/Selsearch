// SPDX-License-Identifier: MIT
#include "ImageSearch.h"

#include "PrivateNetwork.h"

#include <QBuffer>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QLocale>
#include <QPainter>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace selsearch {

const QList<ImageSearch::Engine> &ImageSearch::engines()
{
    static const QList<Engine> list = {
        {QStringLiteral("google_lens"), QStringLiteral("Google Lens"),
         // ep=gsbubb is the "search by image" bubble entry point, re=df the
         // desktop full-page result; st (ms timestamp) is appended per page.
         QStringLiteral("https://lens.google.com/v3/upload?ep=gsbubb&re=df"),
         QStringLiteral("encoded_image"), Engine::FileField, {}, {}},
        {QStringLiteral("yandex"), QStringLiteral("Yandex Images"),
         QStringLiteral("https://yandex.com/images/search?rpt=imageview&format=json&request="
                        "%7B%22blocks%22%3A%5B%7B%22block%22%3A%22b-page_type_search-by-image__link%22%7D%5D%7D"),
         QStringLiteral("upfile"), Engine::NativeUpload, {},
         QStringLiteral("https://yandex.com/images/search")},
        {QStringLiteral("bing"), QStringLiteral("Bing Visual Search"),
         QStringLiteral("https://www.bing.com/images/search?view=detailv2&iss=sbiupload&FORM=SBIIDP"),
         QStringLiteral("imageBin"), Engine::Base64Field,
         {{QStringLiteral("cbir"), QStringLiteral("sbi")}}, {}},
        {QStringLiteral("tineye"), QStringLiteral("TinEye"),
         QStringLiteral("https://tineye.com/api/v1/result_json/?sort=score&order=desc"),
         QStringLiteral("image"), Engine::UploadThenOpen, {},
         QStringLiteral("https://tineye.com/search/%1")},
    };
    return list;
}

std::optional<ImageSearch::Engine> ImageSearch::engine(const QString &id)
{
    for (const Engine &e : engines()) {
        if (e.id == id)
            return e;
    }
    return std::nullopt;
}

QByteArray ImageSearch::encodeJpeg(const QImage &image, int maxSide, int quality)
{
    if (image.isNull())
        return {};
    QImage img = image;
    if (std::max(img.width(), img.height()) > maxSide)
        img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // JPEG has no alpha: flatten onto white instead of the default black.
    if (img.hasAlphaChannel()) {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    } else {
        img = img.convertToFormat(QImage::Format_RGB32);
    }
    img.setDevicePixelRatio(1);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QImageWriter writer(&buf, "jpeg");
    writer.setQuality(quality);
    writer.setOptimizedWrite(true);
    if (!writer.write(img))
        return {};
    return out;
}

static QString jsString(const QString &s)
{
    // JSON-ish string literal that is also safe inside <script>.
    QString out = QStringLiteral("\"");
    for (QChar c : s) {
        switch (c.unicode()) {
        case '"': out += QStringLiteral("\\\""); break;
        case '\\': out += QStringLiteral("\\\\"); break;
        case '<': out += QStringLiteral("\\u003c"); break;
        case '>': out += QStringLiteral("\\u003e"); break;
        case '&': out += QStringLiteral("\\u0026"); break;
        case '\n': out += QStringLiteral("\\n"); break;
        case '\r': out += QStringLiteral("\\r"); break;
        case 0x2028: out += QStringLiteral("\\u2028"); break;
        case 0x2029: out += QStringLiteral("\\u2029"); break;
        default: out += c;
        }
    }
    return out + QLatin1Char('"');
}

QString ImageSearch::buildHtml(const QByteArray &jpeg, const Engine &engine)
{
    const QString b64 = QString::fromLatin1(jpeg.toBase64());
    QString action = engine.action;
    if (engine.id == QLatin1String("google_lens")) {
        const QString hl = QLocale::system().bcp47Name().section(QLatin1Char('-'), 0, 0);
        action += QStringLiteral("&hl=%1&st=%2")
                      .arg(hl.isEmpty() || hl == QLatin1String("C") ? QStringLiteral("en") : hl)
                      .arg(QDateTime::currentMSecsSinceEpoch());
    }
    const QString resultUrl = engine.mode == Engine::UploadThenOpen
        ? engine.resultUrl.arg(QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha1).toHex()))
        : QString();
    const QString name = engine.name.toHtmlEscaped();

    QString fields;
    for (const auto &f : engine.extraFields) {
        fields += QStringLiteral("    <input type=\"hidden\" name=\"%1\" value=\"%2\">\n")
                      .arg(f.first.toHtmlEscaped(), f.second.toHtmlEscaped());
    }
    if (engine.mode == Engine::Base64Field) {
        fields += QStringLiteral("    <input type=\"hidden\" name=\"%1\" value=\"%2\">\n")
                      .arg(engine.field.toHtmlEscaped(), b64);
    } else {
        fields += QStringLiteral("    <input type=\"file\" name=\"%1\" id=\"file\" accept=\"image/jpeg\" hidden>\n")
                      .arg(engine.field.toHtmlEscaped());
    }
    const bool sink = engine.mode == Engine::UploadThenOpen;

    QString html;
    html += QStringLiteral(
        "<!doctype html>\n"
        "<html><head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"referrer\" content=\"no-referrer\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        // Local page: inline script/style only; forms and frames only over
        // https (engines redirect to other hosts of theirs after the upload).
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
        "script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; form-action %1; frame-src %1\">\n"
        "<title>Searching with %2…</title>\n"
        "<style>\n"
        "  :root { color-scheme: light dark; }\n"
        "  body { font: 15px/1.5 system-ui, sans-serif; display: grid; place-items: center;\n"
        "         min-height: 100vh; margin: 0; background: Canvas; color: CanvasText; }\n"
        "  main { text-align: center; padding: 24px; }\n"
        "  img { max-width: 240px; max-height: 160px; border-radius: 8px; opacity: .85; }\n"
        "  button { font: inherit; margin-top: 12px; padding: 6px 16px; border-radius: 8px; }\n"
        "  .hint { opacity: .65; font-size: 13px; }\n"
        "</style>\n"
        "</head><body>\n"
        "<main>\n"
        "  <img alt=\"\" src=\"data:image/jpeg;base64,%3\">\n"
        "  <p id=\"msg\">Uploading image to %2…</p>\n")
                .arg(QStringLiteral("https:"), name, b64);
    html += QStringLiteral(
        "  <form id=\"f\" method=\"post\" enctype=\"multipart/form-data\" action=\"%1\"%2>\n"
        "%3"
        "    <button type=\"submit\" id=\"go\">Search with %4</button>\n"
        "  </form>\n"
        "  <p class=\"hint\">If nothing happens, press the button.</p>\n")
                .arg(action.toHtmlEscaped(), sink ? QStringLiteral(" target=\"sink\"") : QString(), fields, name);
    if (sink)
        html += QStringLiteral("  <iframe name=\"sink\" id=\"sink\" hidden></iframe>\n");
    html += QStringLiteral("</main>\n<script>\n(function () {\n  'use strict';\n");
    if (engine.mode != Engine::Base64Field)
        html += QStringLiteral("  var B64 = %1;\n").arg(jsString(b64));
    html += QStringLiteral("  var RESULT = %1;\n").arg(jsString(resultUrl));
    html += QStringLiteral(
        "  var form = document.getElementById('f');\n"
        "  var input = document.getElementById('file');\n"
        "  function attach() {\n"
        "    if (!input || (input.files && input.files.length)) return;\n"
        "    var bin = atob(B64), bytes = new Uint8Array(bin.length);\n"
        "    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);\n"
        "    var dt = new DataTransfer();\n"
        "    dt.items.add(new File([bytes], 'image.jpg', { type: 'image/jpeg' }));\n"
        "    input.files = dt.files;\n"
        "  }\n"
        "  var sent = false;\n"
        "  function submit() {\n"
        "    attach();\n"
        "    sent = true;\n"
        "    form.submit();\n"
        "  }\n"
        "  form.addEventListener('submit', function (ev) { ev.preventDefault(); submit(); });\n"
        "  if (RESULT) {\n"
        "    // Upload into the hidden frame, then open the results page.\n"
        "    var opened = false;\n"
        "    function open() { if (!opened) { opened = true; location.replace(RESULT); } }\n"
        "    document.getElementById('sink').addEventListener('load', function () { if (sent) open(); });\n"
        "    setTimeout(function () { if (sent) open(); }, 20000);\n"
        "  }\n"
        "  try {\n"
        "    submit();\n"
        "  } catch (e) {\n"
        "    document.getElementById('msg').textContent = 'Could not start the upload automatically.';\n"
        "  }\n"
        "})();\n"
        "</script>\n"
        "</body></html>\n");
    return html;
}

QString ImageSearch::outputDir()
{
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
#ifdef Q_OS_WIN
    if (runtime.isEmpty() || !QFileInfo(runtime).isDir())
        runtime = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#endif
    if (runtime.isEmpty() || !QFileInfo(runtime).isDir())
        runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return runtime + QStringLiteral("/selsearch");
}

int ImageSearch::cleanup(const QString &dir, int maxAgeSecs)
{
    int removed = 0;
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-maxAgeSecs);
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("imgsearch-*.html")}, QDir::Files);
    for (const QFileInfo &fi : files) {
        if (fi.lastModified().toUTC() < cutoff && QFile::remove(fi.absoluteFilePath()))
            ++removed;
    }
    return removed;
}

QUrl ImageSearch::prepare(const QImage &image, const QString &engineId, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return QUrl();
    };
    const auto e = engine(engineId);
    if (!e)
        return fail(QStringLiteral("Unknown image search engine: %1").arg(engineId));
    if (!e->pageBased())
        return fail(QStringLiteral("%1 needs ImageSearch::start()").arg(e->name));
    const QByteArray jpeg = encodeJpeg(image);
    if (jpeg.isEmpty())
        return fail(QStringLiteral("Could not encode the image"));

    const QString dir = outputDir();
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("Cannot create %1").arg(dir));
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    cleanup(dir);

    const QString path = QStringLiteral("%1/imgsearch-%2%3.html")
                             .arg(dir)
                             .arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'))
                             .arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFile::ReadOwner | QFile::WriteOwner))
        return fail(f.errorString());
    const QByteArray html = buildHtml(jpeg, *e).toUtf8();
    if (f.write(html) != html.size()) {
        f.remove();
        return fail(QStringLiteral("Could not write %1").arg(path));
    }
    f.close();
    return QUrl::fromLocalFile(path);
}

QUrl ImageSearch::parseYandexReply(const QByteArray &json)
{
    const QJsonArray blocks = QJsonDocument::fromJson(json).object().value(QLatin1String("blocks")).toArray();
    for (const QJsonValue &b : blocks) {
        const QJsonObject params = b.toObject().value(QLatin1String("params")).toObject();
        const QString id = params.value(QLatin1String("cbirId")).toString();
        if (id.isEmpty())
            continue;
        QUrl url(QStringLiteral("https://yandex.com/images/search"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("rpt"), QStringLiteral("imageview"));
        q.addQueryItem(QStringLiteral("cbir_id"), QString::fromLatin1(QUrl::toPercentEncoding(id)));
        url.setQuery(q);
        return url;
    }
    return {};
}

void ImageSearch::start(const QImage &image, const QString &engineId, QObject *context, Done done)
{
    const auto e = engine(engineId);
    if (!e) {
        QTimer::singleShot(0, context, [done, engineId] {
            done({}, QStringLiteral("Unknown image search engine: %1").arg(engineId));
        });
        return;
    }
    start(image, *e, context, std::move(done));
}

void ImageSearch::start(const QImage &image, const Engine &engine, QObject *context, Done done)
{
    if (engine.pageBased()) {
        QString error;
        const QUrl url = prepare(image, engine.id, &error);
        QTimer::singleShot(0, context, [done, url, error] { done(url, error); });
        return;
    }

    const QByteArray jpeg = encodeJpeg(image);
    if (jpeg.isEmpty()) {
        QTimer::singleShot(0, context, [done] { done({}, QStringLiteral("Could not encode the image")); });
        return;
    }
    // One short-lived, cookie-less network manager per upload, owned by the
    // context so it goes away with it.
    QNetworkAccessManager *nam = createPrivateNetworkAccessManager(context);
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/jpeg"));
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"%1\"; filename=\"image.jpg\"").arg(engine.field));
    part.setBody(jpeg);
    multi->append(part);

    QNetworkRequest req{QUrl(engine.action)};
    preparePrivateRequest(req);
    req.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = nam->post(req, multi);
    multi->setParent(reply);
    const QString engineId = engine.id;
    const QString name = engine.name;
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, nam, done, engineId, name] {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, QStringLiteral("%1: %2").arg(name, reply->errorString()));
            return;
        }
        const QByteArray body = reply->read(4 * 1024 * 1024);
        QUrl url;
        if (engineId == QLatin1String("yandex"))
            url = parseYandexReply(body);
        if (!url.isValid()) {
            done({}, QStringLiteral("%1 did not accept the image").arg(name));
            return;
        }
        done(url, {});
    });
}

}  // namespace selsearch
