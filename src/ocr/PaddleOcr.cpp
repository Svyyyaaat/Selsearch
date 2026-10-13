// SPDX-License-Identifier: MIT
//
// PP-OCRv5 (mobile) on ONNX Runtime: DBNet text detection, then SVTR/CTC line
// recognition, then word boxes derived from the CTC alignment. No OpenCV: the
// few image operations needed (connected components, box unclipping, crops)
// are implemented here on plain buffers or with QImage/QPainter.
//
// Pipeline for one image:
//   1. detect():   resize (long side <= kDetMaxSide, multiples of 32), ImageNet
//                  normalisation in BGR order (as PaddleOCR feeds it), run the
//                  detector, binarise the probability map at 0.3, label 4-
//                  connected components, score each by its mean probability,
//                  fit an axis-aligned box (or a rotated one for skewed lines),
//                  unclip it like DBNet's Vatti offset (d = A * r / L) and map
//                  it back to image pixels.
//   2. recognize: crop every box to height 48 (width proportional), batch
//                  crops of similar width, run the batches concurrently, CTC
//                  greedy decode while remembering the timestep range of each
//                  character.
//   3. words:      split the decoded line on spaces (and on unusually wide
//                  blank gaps); a word's horizontal span comes from the CTC
//                  timesteps of its first and last characters (8 input px per
//                  timestep), tightened to the actual ink in the crop, and is
//                  mapped back through the crop transform to image pixels.
//
// Light-on-dark text needs no special handling: detection and recognition
// read it as well as dark-on-light (see tst_paddleocr, "dark"/"dim" rows), so
// crops are not inverted (SELSEARCH_PPOCR_INVERT=1 does it, for experiments).

#include "OcrEngine.h"

#include "ModelStore.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QTransform>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <exception>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace selsearch {
namespace {

constexpr int kDetMaxSide = 1536;     // long side cap for detection input
constexpr int kDetMinSide = 64;       // PaddleOCR v5 default: upscale below this
constexpr float kDetThresh = 0.3f;    // probability -> binary map
constexpr float kBoxThresh = 0.6f;    // mean probability of a component
constexpr float kUnclipRatio = 1.6f;  // DBNet unclip (PaddleOCR default 1.5)
constexpr int kRecHeight = 48;
constexpr int kRecStride = 8;          // input px per CTC timestep
constexpr int kRecMinWidth = 64;       // narrowest crop fed to the recognizer
constexpr int kRecMaxWidth = 4096;     // wider lines are squeezed
constexpr int kRecBatchPixels = 48 * 320 * 4;  // per-batch input budget; ~30 KB of activations per column
constexpr float kDropScore = 0.5f;     // drop lines whose mean char prob is below
constexpr double kSkewDegrees = 2.5;   // below this, boxes are axis-aligned

Ort::Env &ortEnv()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "selsearch");
    return env;
}

// A text box as an oriented rectangle in image pixels: the point at crop
// coordinates (x, y), 0 <= x <= w, 0 <= y <= h, is origin + u*x + v*y.
struct TextBox {
    QPointF origin;
    QPointF u{1, 0};
    QPointF v{0, 1};
    double w = 0;
    double h = 0;
    float score = 0;
    bool axisAligned() const { return std::abs(u.y()) < 1e-6; }
    QPointF map(double x, double y) const { return origin + u * x + v * y; }
};

struct DecodedChar {
    QString text;
    int firstT = 0;
    int lastT = 0;
    float prob = 0;
    bool space = false;
};

// ----------------------------------------------------------------------------
// Dictionary loading

// PaddleOCR's inference.yml lists the dictionary under PostProcess/character_dict
// as a YAML block sequence. All official PP-OCRv5 dicts use only plain scalars
// and single-quoted scalars ('' escapes a quote), so a line-based reader is
// enough; double-quoted scalars are handled for the common escapes anyway.
QStringList parseYamlCharacterDict(const QByteArray &yaml)
{
    QStringList out;
    const QList<QByteArray> lines = yaml.split('\n');
    bool inDict = false;
    for (QByteArray line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!inDict) {
            if (line.trimmed() == "character_dict:")
                inDict = true;
            continue;
        }
        const qsizetype dash = line.indexOf("- ");
        if (dash < 0 || !line.left(dash).trimmed().isEmpty()) {
            if (line.trimmed().isEmpty())
                continue;
            break;  // end of the sequence
        }
        QByteArray v = line.mid(dash + 2);
        QString s;
        if (v.size() >= 2 && v.startsWith('\'') && v.endsWith('\'')) {
            s = QString::fromUtf8(v.mid(1, v.size() - 2)).replace(QStringLiteral("''"), QStringLiteral("'"));
        } else if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) {
            const QString raw = QString::fromUtf8(v.mid(1, v.size() - 2));
            for (qsizetype i = 0; i < raw.size(); ++i) {
                if (raw[i] == QLatin1Char('\\') && i + 1 < raw.size()) {
                    const QChar e = raw[++i];
                    if (e == QLatin1Char('n'))
                        s += QLatin1Char('\n');
                    else if (e == QLatin1Char('t'))
                        s += QLatin1Char('\t');
                    else if (e == QLatin1Char('u') && i + 4 < raw.size()) {
                        s += QChar(raw.mid(i + 1, 4).toUShort(nullptr, 16));
                        i += 4;
                    } else
                        s += e;
                } else {
                    s += raw[i];
                }
            }
        } else {
            s = QString::fromUtf8(v);
        }
        out << s;
    }
    return out;
}

// Dictionary for a recognition model, in order of preference:
//   1. ONNX custom metadata "character" (RapidOCR conversions; newline separated)
//   2. <model>.txt next to the model (one entry per line, PaddleOCR dict format)
//   3. <model>.yml next to the model (PaddleOCR inference.yml)
QStringList loadDictionary(Ort::Session &session, const QString &onnxPath)
{
    try {
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::ModelMetadata meta = session.GetModelMetadata();
        Ort::AllocatedStringPtr chars = meta.LookupCustomMetadataMapAllocated("character", alloc);
        if (chars) {
            QStringList d = QString::fromUtf8(chars.get()).split(QLatin1Char('\n'));
            if (!d.isEmpty() && d.last().isEmpty())
                d.removeLast();
            if (!d.isEmpty())
                return d;
        }
    } catch (const Ort::Exception &) {
    }
    const QString base = onnxPath.left(onnxPath.size() - 5);  // strip ".onnx"
    QFile txt(base + QStringLiteral(".txt"));
    if (txt.open(QIODevice::ReadOnly)) {
        QStringList d;
        for (QByteArray l : txt.readAll().split('\n')) {
            if (l.endsWith('\r'))
                l.chop(1);
            d << QString::fromUtf8(l);
        }
        while (!d.isEmpty() && d.last().isEmpty())
            d.removeLast();
        if (!d.isEmpty())
            return d;
    }
    QFile yml(base + QStringLiteral(".yml"));
    if (yml.open(QIODevice::ReadOnly))
        return parseYamlCharacterDict(yml.readAll());
    return {};
}

// ----------------------------------------------------------------------------
// Detection post-processing

struct Component {
    int count = 0;
    double sumP = 0;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int minX = INT_MAX, minY = INT_MAX, maxX = -1, maxY = -1;
    int pixelStart = 0;  // into the flat pixel list
};

// Label 4-connected components of (prob > thresh) and turn each into a box in
// detection-map coordinates.
QList<TextBox> boxesFromProbMap(const float *prob, int w, int h)
{
    std::vector<int> label(size_t(w) * h, 0);
    std::vector<int> pixels;  // flat pixel indices, grouped by component
    pixels.reserve(size_t(w) * h / 8);
    std::vector<int> stack;
    std::vector<Component> comps;

    for (int start = 0; start < w * h; ++start) {
        if (label[start] || prob[start] <= kDetThresh)
            continue;
        Component c;
        c.pixelStart = int(pixels.size());
        const int id = int(comps.size()) + 1;
        label[start] = id;
        stack.push_back(start);
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            pixels.push_back(p);
            const int x = p % w, y = p / w;
            c.count++;
            c.sumP += prob[p];
            c.sx += x;
            c.sy += y;
            c.sxx += double(x) * x;
            c.syy += double(y) * y;
            c.sxy += double(x) * y;
            c.minX = std::min(c.minX, x);
            c.maxX = std::max(c.maxX, x);
            c.minY = std::min(c.minY, y);
            c.maxY = std::max(c.maxY, y);
            auto visit = [&](int q) {
                if (!label[q] && prob[q] > kDetThresh) {
                    label[q] = id;
                    stack.push_back(q);
                }
            };
            if (x > 0)
                visit(p - 1);
            if (x + 1 < w)
                visit(p + 1);
            if (y > 0)
                visit(p - w);
            if (y + 1 < h)
                visit(p + w);
        }
        comps.push_back(c);
    }

    QList<TextBox> boxes;
    for (const Component &c : comps) {
        if (c.count < 6)
            continue;
        const float score = float(c.sumP / c.count);
        if (score < kBoxThresh)
            continue;
        const double bw = c.maxX - c.minX + 1, bh = c.maxY - c.minY + 1;
        if (std::min(bw, bh) < 3)
            continue;

        TextBox b;
        b.score = score;
        // Orientation from second moments. Only elongated components (lines of
        // text) are worth rotating; short blobs give unstable angles.
        const double n = c.count, mx = c.sx / n, my = c.sy / n;
        const double mu20 = c.sxx / n - mx * mx, mu02 = c.syy / n - my * my, mu11 = c.sxy / n - mx * my;
        const double theta = 0.5 * std::atan2(2 * mu11, mu20 - mu02);
        const bool skewed = std::abs(theta) * 180.0 / M_PI > kSkewDegrees && std::abs(theta) * 180.0 / M_PI < 30.0
                            && bw > 3 * bh;
        if (!skewed) {
            b.origin = QPointF(c.minX, c.minY);
            b.w = bw;
            b.h = bh;
        } else {
            // Rotated rectangle along the principal axis (a close stand-in for
            // the minimum-area rectangle for line-shaped components).
            const double ux = std::cos(theta), uy = std::sin(theta);
            double minU = 1e18, maxU = -1e18, minV = 1e18, maxV = -1e18;
            for (int i = 0; i < c.count; ++i) {
                const int p = pixels[size_t(c.pixelStart) + i];
                const double x = p % w + 0.5, y = p / w + 0.5;
                const double pu = x * ux + y * uy, pv = -x * uy + y * ux;
                minU = std::min(minU, pu - 0.5);
                maxU = std::max(maxU, pu + 0.5);
                minV = std::min(minV, pv - 0.5);
                maxV = std::max(maxV, pv + 0.5);
            }
            b.u = QPointF(ux, uy);
            b.v = QPointF(-uy, ux);
            b.origin = b.u * minU + b.v * minV;
            b.w = maxU - minU;
            b.h = maxV - minV;
        }
        // Unclip: DBNet shrinks text regions by D = A(1 - r^2)/L during
        // training; expanding the polygon by A * ratio / L restores it.
        const double d = b.w * b.h * kUnclipRatio / (2.0 * (b.w + b.h));
        b.origin -= b.u * d + b.v * d;
        b.w += 2 * d;
        b.h += 2 * d;
        if (std::min(b.w, b.h) < 5)
            continue;
        boxes << b;
    }
    return boxes;
}

// ----------------------------------------------------------------------------
// Recognition helpers

struct Crop {
    int boxIndex = -1;
    int width = 0;               // resized width (height is kRecHeight)
    std::vector<float> gray;     // kRecHeight x width luminance 0..255
    QImage image;                // RGB888, kRecHeight x width
};

QImage sampleCrop(const QImage &rgb, const TextBox &b, int outW)
{
    if (b.axisAligned()) {
        const QRect r = QRectF(b.origin, QSizeF(b.w, b.h)).toAlignedRect().intersected(rgb.rect());
        if (r.isEmpty())
            return {};
        return rgb.copy(r).scaled(outW, kRecHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);
    }
    QImage out(outW, kRecHeight, QImage::Format_RGB888);
    const QPoint c = b.map(b.w / 2, b.h / 2).toPoint();
    out.fill(rgb.pixelColor(std::clamp(c.x(), 0, rgb.width() - 1), std::clamp(int(b.origin.y()), 0, rgb.height() - 1)));
    // target (x, y) -> source origin + u * x * sx + v * y * sy
    const double sx = b.w / outW, sy = b.h / kRecHeight;
    const QTransform targetToSource(b.u.x() * sx, b.u.y() * sx, b.v.x() * sy, b.v.y() * sy, b.origin.x(), b.origin.y());
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(targetToSource.inverted());
    p.drawImage(QPointF(0, 0), rgb);
    p.end();
    return out;
}

bool isCjk(char32_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF)
           || (c >= 0x20000 && c <= 0x2FA1F);
}

class PaddleOcrEngine final : public OcrEngine {
public:
    explicit PaddleOcrEngine(const QString &modelDir)
        : m_dir(modelDir)
    {
        // Development knobs for experiments with selsearch-ocr-dump; not user
        // configuration:
        //   SELSEARCH_PPOCR_DET_SIDE=N     detector long-side cap (default 1536)
        //   SELSEARCH_PPOCR_REC_MODEL=id   force a (present) recognition model
        //   SELSEARCH_PPOCR_INVERT=1       invert dark crops before recognition
        //   SELSEARCH_PPOCR_REC_WORKERS=N  concurrent recognition batches
        //   SELSEARCH_PPOCR_DEBUG=1        per-stage timings on stderr
        if (const int side = qEnvironmentVariableIntValue("SELSEARCH_PPOCR_DET_SIDE"); side >= 320)
            m_detMaxSide = side;
        m_invertDark = qEnvironmentVariableIntValue("SELSEARCH_PPOCR_INVERT") != 0;
        m_recOverride = qEnvironmentVariable("SELSEARCH_PPOCR_REC_MODEL");
        if (const int n = qEnvironmentVariableIntValue("SELSEARCH_PPOCR_REC_WORKERS"); n > 0)
            m_recWorkers = std::min(n, 16);
        m_debug = qEnvironmentVariableIsSet("SELSEARCH_PPOCR_DEBUG");
    }

    QString id() const override { return QStringLiteral("paddle"); }

    Ort::RunOptions runOptions() const
    {
        Ort::RunOptions ro;
        // Hand the arena's chunks back after every run: input shapes differ
        // from run to run, so cached chunks would mostly be dead weight in a
        // resident daemon.
        ro.AddConfigEntry("memory.enable_memory_arena_shrinkage", "cpu:0");
        return ro;
    }

    // Leave one core for the compositor and the overlay's rendering.
    static int threadCount()
    {
        const int hw = int(std::max(1u, std::thread::hardware_concurrency()));
        return std::clamp(hw - 1, 1, 4);
    }

    bool isAvailable() const override
    {
        const ModelSpec *det = ModelStore::findSpec(ModelStore::detectionModelId());
        return det && ModelStore::isPresent(m_dir, *det) && !ModelStore::presentRecognitionModels(m_dir).isEmpty();
    }

    void unload() override
    {
        m_det.reset();
        m_rec.reset();
        m_recId.clear();
        m_dict.clear();
        m_dict.squeeze();
    }

    OcrResult recognize(const QImage &input, const QStringList &languages) override
    {
        OcrResult result;
        result.engine = id();
        if (input.isNull() || input.width() < 4 || input.height() < 4)
            return result;
        try {
            QElapsedTimer t;
            t.start();
            if (!ensureSessions(languages, result.error))
                return result;
            const qint64 tLoad = t.restart();
            const QImage rgb = input.convertToFormat(QImage::Format_RGB888);
            const QList<TextBox> boxes = detect(rgb);
            const qint64 tDet = t.restart();
            result.lines = recognizeBoxes(rgb, boxes);
            const qint64 tRec = t.restart();
            sortReadingOrder(result.lines);
            if (m_debug)
                qInfo("ppocr: %s load=%lldms det=%lldms (%d boxes) rec=%lldms (%lld input columns in %d batches)",
                      qPrintable(m_recId), tLoad, tDet, int(boxes.size()), tRec, m_statColumns, m_statBatches);
        } catch (const Ort::Exception &e) {
            result.lines.clear();
            result.error = QStringLiteral("onnxruntime: %1").arg(QString::fromUtf8(e.what()));
        } catch (const std::exception &e) {
            result.lines.clear();
            result.error = QStringLiteral("ppocr: %1").arg(QString::fromUtf8(e.what()));
        }
        return result;
    }

private:
    std::unique_ptr<Ort::Session> createSession(const QString &path, int intraThreads)
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(intraThreads);
        opts.SetInterOpNumThreads(1);
        opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Input shapes change with every image and crop batch, so memory
        // patterns do not help. The CPU arena stays on (it is measurably faster
        // with concurrent batches) but is shrunk after every run, see
        // runOptions().
        opts.DisableMemPattern();
        // Do not leave worker threads spinning after a run.
        opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
#ifdef _WIN32
        // ORTCHAR_T is wchar_t on Windows.
        const std::wstring p = path.toStdWString();
        return std::make_unique<Ort::Session>(ortEnv(), p.c_str(), opts);
#else
        const QByteArray p = QFile::encodeName(path);
        return std::make_unique<Ort::Session>(ortEnv(), p.constData(), opts);
#endif
    }

    bool ensureSessions(const QStringList &languages, QString &error)
    {
        const ModelSpec *det = ModelStore::findSpec(ModelStore::detectionModelId());
        if (!det || !ModelStore::isPresent(m_dir, *det)) {
            error = QStringLiteral("PP-OCR detection model missing in %1").arg(m_dir);
            return false;
        }
        // Use the best *present* recognizer for the request; never download.
        const QStringList present = ModelStore::presentRecognitionModels(m_dir);
        const QString recId = present.contains(m_recOverride) ? m_recOverride
                                                              : ModelStore::bestRecognitionModel(languages, present);
        if (recId.isEmpty()) {
            error = QStringLiteral("no PP-OCR recognition model in %1").arg(m_dir);
            return false;
        }
        if (!m_det)
            m_det = createSession(m_dir + QLatin1Char('/') + det->onnxFile(), threadCount());
        if (!m_rec || m_recId != recId) {
            m_rec.reset();
            const QString path = m_dir + QLatin1Char('/') + ModelStore::findSpec(recId)->onnxFile();
            // Recognition parallelises across batches instead of inside one:
            // m_recWorkers threads each run a batch with a single intra-op
            // thread. Same thread budget, ~1.3x faster than intra-op
            // parallelism on the narrow 48 px tensors.
            m_rec = createSession(path, m_recWorkers > 1 ? 1 : threadCount());
            Ort::AllocatorWithDefaultOptions alloc;
            m_recIn = m_rec->GetInputNameAllocated(0, alloc).get();
            m_recOut = m_rec->GetOutputNameAllocated(0, alloc).get();
            m_dict = loadDictionary(*m_rec, path);
            const auto shape = m_rec->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            const qsizetype classes = shape.empty() ? -1 : qsizetype(shape.back());
            // CTC classes: blank + dictionary (+ space when use_space_char).
            if (classes == m_dict.size() + 2)
                m_dict << QStringLiteral(" ");
            else if (classes > 0 && classes != m_dict.size() + 1) {
                error = QStringLiteral("%1: dictionary has %2 entries but the model outputs %3 classes")
                            .arg(recId)
                            .arg(m_dict.size())
                            .arg(classes);
                m_rec.reset();
                m_dict.clear();
                return false;
            }
            m_recId = recId;
        }
        return true;
    }

    // ---- detection -------------------------------------------------------

    QList<TextBox> detect(const QImage &rgb)
    {
        const int W = rgb.width(), H = rgb.height();
        double scale = std::min(1.0, double(m_detMaxSide) / std::max(W, H));
        if (std::min(W, H) * scale < kDetMinSide)
            scale = std::min(double(kDetMinSide) / std::min(W, H), 4096.0 / std::max(W, H));
        const int dw = std::max(32, int(std::lround(W * scale / 32.0)) * 32);
        const int dh = std::max(32, int(std::lround(H * scale / 32.0)) * 32);
        const QImage img = (dw == W && dh == H) ? rgb
                                                : rgb.scaled(dw, dh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                                      .convertToFormat(QImage::Format_RGB888);

        // NCHW, BGR channel order, ImageNet mean/std (PaddleOCR reads images as
        // BGR and applies the mean/std lists in that channel order).
        std::vector<float> input(size_t(3) * dw * dh);
        const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
        float lut[3][256];
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i)
                lut[c][i] = (i / 255.0f - mean[c]) / stdv[c];
        const size_t plane = size_t(dw) * dh;
        for (int y = 0; y < dh; ++y) {
            const uchar *row = img.constScanLine(y);
            float *b = input.data() + size_t(y) * dw, *g = b + plane, *r = g + plane;
            for (int x = 0; x < dw; ++x) {
                r[x] = lut[2][row[3 * x]];
                g[x] = lut[1][row[3 * x + 1]];
                b[x] = lut[0][row[3 * x + 2]];
            }
        }
        const std::array<int64_t, 4> shape{1, 3, dh, dw};
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor =
            Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions alloc;
        const auto inNamePtr = m_det->GetInputNameAllocated(0, alloc);
        const auto outNamePtr = m_det->GetOutputNameAllocated(0, alloc);
        const char *inName = inNamePtr.get(), *outName = outNamePtr.get();
        auto outputs = m_det->Run(runOptions(), &inName, &tensor, 1, &outName, 1);
        input.clear();
        input.shrink_to_fit();

        const auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int ow = int(outShape.back()), oh = int(outShape[outShape.size() - 2]);
        QList<TextBox> boxes = boxesFromProbMap(outputs[0].GetTensorData<float>(), ow, oh);

        // Detection map -> image pixels.
        const double fx = double(W) / ow, fy = double(H) / oh;
        QList<TextBox> out;
        for (TextBox b : boxes) {
            const QPointF o = b.origin, pu = b.map(b.w, 0), pv = b.map(0, b.h);
            const QPointF O(o.x() * fx, o.y() * fy), U(pu.x() * fx, pu.y() * fy), V(pv.x() * fx, pv.y() * fy);
            const QPointF du = U - O, dv = V - O;
            b.w = std::hypot(du.x(), du.y());
            b.h = std::hypot(dv.x(), dv.y());
            if (b.w < 2 || b.h < 4)
                continue;
            b.origin = O;
            b.u = du / b.w;
            b.v = dv / b.h;
            if (b.axisAligned()) {
                // Clip to the image.
                QRectF r = QRectF(b.origin, QSizeF(b.w, b.h)).intersected(QRectF(rgb.rect()));
                if (r.width() < 2 || r.height() < 4)
                    continue;
                b.origin = r.topLeft();
                b.w = r.width();
                b.h = r.height();
            }
            out << b;
        }
        return out;
    }

    // ---- recognition -----------------------------------------------------

    QList<OcrLine> recognizeBoxes(const QImage &rgb, const QList<TextBox> &boxes)
    {
        std::vector<Crop> crops;
        crops.reserve(size_t(boxes.size()));
        for (int i = 0; i < boxes.size(); ++i) {
            const TextBox &b = boxes[i];
            Crop c;
            c.boxIndex = i;
            c.width = std::clamp(int(std::ceil(kRecHeight * b.w / b.h)), 8, kRecMaxWidth);
            c.image = sampleCrop(rgb, b, c.width);
            if (c.image.isNull())
                continue;
            c.width = c.image.width();
            c.gray.resize(size_t(kRecHeight) * c.width);
            double sum = 0;
            for (int y = 0; y < kRecHeight; ++y) {
                const uchar *row = c.image.constScanLine(y);
                for (int x = 0; x < c.width; ++x) {
                    const float l = 0.299f * row[3 * x] + 0.587f * row[3 * x + 1] + 0.114f * row[3 * x + 2];
                    c.gray[size_t(y) * c.width + x] = l;
                    sum += l;
                }
            }
            if (m_invertDark && sum / c.gray.size() < 100.0)
                c.image.invertPixels();
            crops.push_back(std::move(c));
        }

        // Batch crops of similar width (sorted by width) so little padding is
        // wasted, within a fixed pixel budget per batch.
        std::vector<int> order(crops.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return crops[a].width < crops[b].width; });

        std::vector<std::vector<int>> batches;
        m_statColumns = 0;
        size_t i = 0;
        while (i < order.size()) {
            size_t j = i + 1;
            const int minW = std::max(kRecMinWidth, crops[order[i]].width);
            while (j < order.size()) {
                const int wj = std::max(kRecMinWidth, crops[order[j]].width);
                if (wj > minW * 1.3 + 32 || qint64(wj) * kRecHeight * qint64(j - i + 1) > kRecBatchPixels)
                    break;
                ++j;
            }
            batches.emplace_back(order.begin() + i, order.begin() + j);
            m_statColumns += qint64(j - i) * std::max(kRecMinWidth, crops[order[j - 1]].width);
            i = j;
        }
        m_statBatches = int(batches.size());

        // Widest batches first so the workers finish together.
        std::reverse(batches.begin(), batches.end());
        std::vector<QList<OcrLine>> results(batches.size());
        std::atomic<size_t> next{0};
        std::mutex errorMutex;
        std::exception_ptr error;
        auto worker = [&] {
#ifdef _WIN32
            // Linux threads inherit the OCR thread's nice value; Windows ones do not.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            for (;;) {
                const size_t b = next++;
                if (b >= batches.size())
                    return;
                try {
                    results[b] = runRecBatch(crops, batches[b], boxes);
                } catch (...) {
                    std::lock_guard lock(errorMutex);
                    if (!error)
                        error = std::current_exception();
                    next = batches.size();
                    return;
                }
                for (int k : batches[b]) {  // each crop belongs to exactly one batch
                    crops[k].image = QImage();
                    crops[k].gray = {};
                }
            }
        };
        const int nWorkers = std::min<int>(m_recWorkers, int(batches.size()));
        if (nWorkers <= 1) {
            worker();
        } else {
            std::vector<std::thread> threads;
            for (int t = 0; t < nWorkers; ++t)
                threads.emplace_back(worker);
            for (auto &t : threads)
                t.join();
        }
        if (error)
            std::rethrow_exception(error);
        QList<OcrLine> lines;
        for (auto &r : results)
            lines << r;
        return lines;
    }

    QList<OcrLine> runRecBatch(const std::vector<Crop> &crops, const std::vector<int> &batch,
                               const QList<TextBox> &boxes) const
    {
        QList<OcrLine> lines;
        int maxW = kRecMinWidth;
        for (int k : batch)
            maxW = std::max(maxW, crops[k].width);
        maxW = (maxW + kRecStride - 1) / kRecStride * kRecStride;
        const int n = int(batch.size());
        const size_t plane = size_t(kRecHeight) * maxW;
        std::vector<float> input(size_t(n) * 3 * plane, 0.0f);  // 0 == mid grey after normalisation
        for (int bi = 0; bi < n; ++bi) {
            const Crop &c = crops[batch[bi]];
            float *b = input.data() + size_t(bi) * 3 * plane, *g = b + plane, *r = g + plane;
            for (int y = 0; y < kRecHeight; ++y) {
                const uchar *row = c.image.constScanLine(y);
                for (int x = 0; x < c.width; ++x) {
                    const size_t o = size_t(y) * maxW + x;
                    r[o] = row[3 * x] / 127.5f - 1.0f;
                    g[o] = row[3 * x + 1] / 127.5f - 1.0f;
                    b[o] = row[3 * x + 2] / 127.5f - 1.0f;
                }
            }
        }
        const std::array<int64_t, 4> shape{n, 3, kRecHeight, maxW};
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor =
            Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), shape.size());
        const char *inName = m_recIn.c_str(), *outName = m_recOut.c_str();
        auto outputs = m_rec->Run(runOptions(), &inName, &tensor, 1, &outName, 1);
        input.clear();
        input.shrink_to_fit();

        const auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();  // N x T x C
        const int T = int(outShape[1]), C = int(outShape[2]);
        const float *data = outputs[0].GetTensorData<float>();
        for (int bi = 0; bi < n; ++bi) {
            const Crop &c = crops[batch[bi]];
            const int usedT = std::min(T, (c.width + kRecStride - 1) / kRecStride);
            const std::vector<DecodedChar> chars = ctcDecode(data + size_t(bi) * T * C, usedT, C);
            std::optional<OcrLine> line = buildLine(chars, c, boxes[c.boxIndex]);
            if (line)
                lines << *line;
        }
        return lines;
    }

    std::vector<DecodedChar> ctcDecode(const float *probs, int T, int C) const
    {
        std::vector<DecodedChar> out;
        int prev = 0;
        for (int t = 0; t < T; ++t) {
            const float *row = probs + size_t(t) * C;
            const int idx = int(std::max_element(row, row + C) - row);
            if (idx != 0 && idx == prev && !out.empty()) {
                out.back().lastT = t;
                out.back().prob = std::max(out.back().prob, row[idx]);
            } else if (idx != 0 && idx - 1 < m_dict.size()) {
                DecodedChar ch;
                ch.text = m_dict[idx - 1];
                ch.firstT = ch.lastT = t;
                ch.prob = row[idx];
                ch.space = ch.text.trimmed().isEmpty();
                out.push_back(ch);
            }
            prev = idx;
        }
        return out;
    }

    std::optional<OcrLine> buildLine(const std::vector<DecodedChar> &chars, const Crop &crop, const TextBox &box) const
    {
        if (chars.empty())
            return std::nullopt;
        float probSum = 0;
        int nonSpace = 0;
        for (const auto &ch : chars)
            if (!ch.space) {
                probSum += ch.prob;
                ++nonSpace;
            }
        if (nonSpace == 0 || probSum / nonSpace < kDropScore)
            return std::nullopt;

        // Ink profile of the crop: contrast against the background luminance
        // (median of the top and bottom rows, which the unclipped box leaves as
        // margin), per column and per row.
        const int W = crop.width;
        std::vector<float> border;
        for (int y : {0, 1, kRecHeight - 2, kRecHeight - 1})
            for (int x = 0; x < W; ++x)
                border.push_back(crop.gray[size_t(y) * W + x]);
        std::nth_element(border.begin(), border.begin() + border.size() / 2, border.end());
        const float bg = border[border.size() / 2];
        std::vector<float> colInk(W, 0.0f), rowInk(kRecHeight, 0.0f);
        float maxInk = 0;
        for (int y = 0; y < kRecHeight; ++y)
            for (int x = 0; x < W; ++x) {
                const float d = std::abs(crop.gray[size_t(y) * W + x] - bg);
                colInk[x] = std::max(colInk[x], d);
                rowInk[y] = std::max(rowInk[y], d);
                maxInk = std::max(maxInk, d);
            }
        const float inkThr = std::max(20.0f, 0.4f * maxInk);
        int top = 0, bottom = kRecHeight - 1;
        while (top < kRecHeight - 1 && rowInk[top] < inkThr)
            ++top;
        while (bottom > top && rowInk[bottom] < inkThr)
            --bottom;
        if (bottom - top < 4) {  // no clear ink (low contrast): use the whole box
            top = 0;
            bottom = kRecHeight - 1;
        }
        // Longest run of ink-free columns in [a, b).
        auto blankRun = [&](int a, int b) {
            int best = 0, run = 0;
            for (int x = std::max(0, a); x < std::min(W, b); ++x) {
                run = colInk[x] < inkThr ? run + 1 : 0;
                best = std::max(best, run);
            }
            return best;
        };

        // Typical distance between consecutive characters, to spot wide blank
        // gaps that the recognizer did not turn into a space (table columns,
        // toolbar labels).
        std::vector<int> steps;
        for (size_t i = 1; i < chars.size(); ++i)
            if (!chars[i].space && !chars[i - 1].space)
                steps.push_back(chars[i].firstT - chars[i - 1].firstT);
        int medianStep = 3;
        if (!steps.empty()) {
            std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
            medianStep = std::max(1, steps[steps.size() / 2]);
        }
        const int gapSplit = std::max(6, medianStep * 3);
        // CJK text has no word spaces, so a run of CJK characters stays one
        // word. The zh/ja model often drops the space between CJK and Latin
        // ("Linux 下载" -> "Linux下载"); at a script change a word break is
        // taken from the ink instead: an empty stretch wider than ~0.25 em
        // (glyph gaps are ~0.1 em). Between two CJK characters the ink is no
        // help (narrow kana leave gaps as wide as a space), so only spaces the
        // model emits split there.
        const int scriptChangeBlankSplit = std::max(3, int(0.25 * (bottom - top + 1)));

        struct WordSpan {
            QString text;
            int firstT, lastT;
            float probSum = 0;
            int n = 0;
        };
        std::vector<WordSpan> words;
        bool newWord = true;
        bool prevCjk = false;
        for (size_t i = 0; i < chars.size(); ++i) {
            const DecodedChar &ch = chars[i];
            if (ch.space) {
                newWord = true;
                continue;
            }
            const QList<uint> ucs = ch.text.toUcs4();
            const bool cjk = !ucs.isEmpty() && isCjk(ucs.first());
            if (!newWord && !words.empty()) {
                const int gap = ch.firstT - words.back().lastT;
                if (cjk && prevCjk) {
                    newWord = false;
                } else if (cjk != prevCjk) {
                    const int a = words.back().lastT * kRecStride + kRecStride / 2;
                    const int b = ch.firstT * kRecStride + kRecStride / 2;
                    newWord = blankRun(a, b) >= scriptChangeBlankSplit;
                } else {
                    newWord = gap > gapSplit;
                }
            }
            if (newWord || words.empty()) {
                words.push_back({ch.text, ch.firstT, ch.lastT, ch.prob, 1});
                newWord = false;
            } else {
                words.back().text += ch.text;
                words.back().lastT = ch.lastT;
                words.back().probSum += ch.prob;
                words.back().n++;
            }
            prevCjk = cjk;
        }
        if (words.empty())
            return std::nullopt;

        const double sx = box.w / W, sy = box.h / kRecHeight;
        OcrLine line;
        for (size_t i = 0; i < words.size(); ++i) {
            const WordSpan &ws = words[i];
            // CTC span: timestep t covers input columns [t*8, t*8+8).
            const int L = ws.firstT * kRecStride, R = std::min(W, (ws.lastT + 1) * kRecStride);
            // Search for ink up to half-way to the neighbouring words' spans,
            // and at most about one character beyond the CTC span (so an icon
            // or border inside the line box does not stretch the word).
            const int slack = kRecStride * std::max(2, medianStep);
            const int prevR = i == 0 ? 0 : (words[i - 1].lastT + 1) * kRecStride;
            const int nextL = i + 1 == words.size() ? W : words[i + 1].firstT * kRecStride;
            const int lo = std::max(L - slack, i == 0 ? 0 : (prevR + L) / 2);
            const int hi = std::min(R + slack, i + 1 == words.size() ? W : (R + nextL) / 2);
            int x0 = -1, x1 = -1;
            for (int x = std::max(0, lo); x < std::min(W, hi); ++x)
                if (colInk[x] >= inkThr) {
                    if (x0 < 0)
                        x0 = x;
                    x1 = x;
                }
            if (x0 < 0 || x1 - x0 < (R - L) / 3) {  // ink search failed: trust CTC
                x0 = L;
                x1 = R - 1;
            }
            const double cx0 = x0 * sx, cx1 = (x1 + 1) * sx;
            const double cy0 = std::max(0.0, (top - 1) * sy), cy1 = std::min(box.h, (bottom + 2) * sy);
            QPolygonF poly;
            poly << box.map(cx0, cy0) << box.map(cx1, cy0) << box.map(cx1, cy1) << box.map(cx0, cy1);
            line.words << OcrWord{ws.text, poly.boundingRect(), ws.probSum / std::max(1, ws.n)};
        }
        return line;
    }

    QString m_dir;
    int m_detMaxSide = kDetMaxSide;
    bool m_invertDark = false;
    qint64 m_statColumns = 0;
    int m_statBatches = 0;
    QString m_recOverride;
    std::unique_ptr<Ort::Session> m_det;
    std::unique_ptr<Ort::Session> m_rec;
    QString m_recId;
    std::string m_recIn, m_recOut;
    int m_recWorkers = threadCount();
    bool m_debug = false;
    QStringList m_dict;
};

}  // namespace

std::unique_ptr<OcrEngine> createPaddleOcrEngine(const QString &modelDir)
{
    return std::make_unique<PaddleOcrEngine>(modelDir);
}

}  // namespace selsearch
