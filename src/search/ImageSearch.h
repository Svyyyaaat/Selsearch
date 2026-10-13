// SPDX-License-Identifier: MIT
#pragma once

// Search the web with an image (a screen region) on Google Lens, Yandex,
// Bing Visual Search or TinEye.
//
// PRIVACY: this uploads the image to the chosen engine, and only that engine.
// It runs only when the user asks for it (the "Search image" action) or when
// they turned on automatic image search (lens.on_select = "search_image").
// Nothing is uploaded at preparation time: prepare() only writes a local
// page; the upload happens when that page is opened.
//
// How: engines only accept uploads from their own web forms, so prepare()
// writes a self-contained HTML page to $XDG_RUNTIME_DIR/selsearch/
// (mode 0600) that embeds the JPEG as base64, rebuilds it as a File with
// DataTransfer, puts it into a multipart <form> that mirrors the engine's
// upload form and submits it. The engine sees an ordinary browser upload, so
// this works the same in the embedded results panel and in the user's browser
// (file:// URL). The page also shows "Uploading image to <Engine>…" and a
// manual button in case scripts are blocked or slow. Pages older than ten
// minutes are deleted on the next prepare().
//
// Yandex is the exception: its upload form no longer leads to results (it
// answers with the landing page), and the endpoint that does work returns
// JSON that a page can't read cross-origin (no CORS). For it, start() uploads
// from the daemon itself (no cookies, generic User-Agent) and resolves with
// the plain results URL. start() handles every engine, so callers use it.
//
// Upload endpoints (checked 2026-10 with curl multipart POSTs of a JPEG, and
// by opening the generated pages in headless Chromium):
//   google_lens  POST https://lens.google.com/v3/upload  file "encoded_image"
//                -> 303 to google.com/search?vsrid=...&udm=26 (the Lens results
//                page; from a datacenter IP Google then shows its captcha).
//   bing         POST https://www.bing.com/images/search?view=detailv2&iss=sbiupload
//                text "imageBin" (base64 JPEG) + "cbir=sbi"
//                -> 302 to view=detailV2&insightsToken=bcid_... From the test
//                network Bing then redirected every visual-search page
//                (including documented imgurl: searches) to /images, so the
//                final results page could not be confirmed.
//   yandex       POST https://yandex.com/images/search?rpt=imageview&format=json
//                &request={"blocks":[{"block":"b-page_type_search-by-image__link"}]}
//                file "upfile" -> JSON with params.cbirId; results page is
//                /images/search?rpt=imageview&cbir_id=<id> (confirmed: similar
//                images list). The plain form POST returns the landing page.
//   tineye       POST https://tineye.com/api/v1/result_json/  file "image"
//                -> JSON whose query_hash is the SHA-1 of the uploaded bytes;
//                the results page is https://tineye.com/search/<sha1>. The
//                old POST /search form now answers 405, so the page posts into
//                a hidden iframe and then navigates to the results page
//                (confirmed end to end in Chromium).

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

class QObject;

namespace selsearch {

class ImageSearch {
public:
    struct Engine {
        enum Mode {
            FileField,       // <input type=file name=field>
            Base64Field,     // <input type=hidden name=field value=BASE64>
            UploadThenOpen,  // file field posted to a hidden iframe, then open resultUrl
            NativeUpload,    // uploaded by start() itself; the JSON reply names the results page
        };
        QString id;      // google_lens | yandex | bing | tineye
        QString name;    // "Google Lens"
        QString action;  // form action URL
        QString field;   // name of the image field
        Mode mode = FileField;
        QList<QPair<QString, QString>> extraFields;  // hidden inputs
        QString resultUrl;  // UploadThenOpen: "%1" = SHA-1 hex of the JPEG
        bool pageBased() const { return mode != NativeUpload; }
    };

    static const QList<Engine> &engines();
    static std::optional<Engine> engine(const QString &id);

    // Downscale to at most `maxSide` px on the long side (never upscales),
    // flatten alpha onto white, encode as JPEG at `quality`.
    static QByteArray encodeJpeg(const QImage &image, int maxSide = 1000, int quality = 90);
    // The auto-submitting page for `jpeg` on `engine`.
    static QString buildHtml(const QByteArray &jpeg, const Engine &engine);

    // Encode, write the page and return its file:// URL (invalid on error,
    // including for engines that are not pageBased()).
    static QUrl prepare(const QImage &image, const QString &engineId, QString *error = nullptr);

    // The entry point for callers: resolves `done` with the URL to open (the
    // local page, or for NativeUpload engines the engine's results page after
    // uploading), or an invalid URL and an error message. `done` runs on
    // `context`'s thread, never synchronously, and not at all when `context`
    // is destroyed first.
    using Done = std::function<void(const QUrl &url, const QString &error)>;
    static void start(const QImage &image, const QString &engineId, QObject *context, Done done);
    static void start(const QImage &image, const Engine &engine, QObject *context, Done done);

    // Yandex: the results URL from the JSON upload reply; invalid when the
    // reply has no cbirId.
    static QUrl parseYandexReply(const QByteArray &json);

    // $XDG_RUNTIME_DIR/selsearch (created 0700 on demand).
    static QString outputDir();
    // Delete imgsearch-*.html pages in `dir` older than `maxAgeSecs`.
    // Returns how many were removed.
    static int cleanup(const QString &dir, int maxAgeSecs = 600);
};

}  // namespace selsearch
