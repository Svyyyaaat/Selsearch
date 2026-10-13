// SPDX-License-Identifier: MIT
//
// In-process screen capture over the compositor's own Wayland protocols.
// Prefers ext-image-copy-capture-v1 (staging, supported by Hyprland, sway 1.10+,
// niri, ...), falls back to wlr-screencopy-unstable-v1. Uses a private
// wl_display connection so it can run on a worker thread.

#include "Capture.h"
#include "ImageTransform.h"

#include <QElapsedTimer>
#include <QLoggingCategory>

#include <wayland-client.h>

#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "wlr-screencopy-unstable-v1-client.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcCapture, "selsearch.capture")

namespace selsearch {
namespace {

struct ShmBuffer {
    wl_buffer *buffer = nullptr;
    void *data = MAP_FAILED;
    size_t size = 0;
    int width = 0, height = 0, stride = 0;
    uint32_t format = 0;

    bool create(wl_shm *shm, int w, int h, int s, uint32_t fmt)
    {
        width = w;
        height = h;
        stride = s;
        format = fmt;
        size = size_t(stride) * size_t(height);
        int fd = memfd_create("selsearch-capture", MFD_CLOEXEC);
        if (fd < 0)
            return false;
        if (ftruncate(fd, off_t(size)) < 0) {
            close(fd);
            return false;
        }
        data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return false;
        }
        wl_shm_pool *pool = wl_shm_create_pool(shm, fd, int32_t(size));
        buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
        wl_shm_pool_destroy(pool);
        close(fd);
        return buffer != nullptr;
    }
    ~ShmBuffer()
    {
        if (buffer)
            wl_buffer_destroy(buffer);
        if (data != MAP_FAILED)
            munmap(data, size);
    }
};

// Returns a QImage format for a wl_shm format, or Format_Invalid.
QImage::Format qtFormat(uint32_t fmt)
{
    switch (fmt) {
    case WL_SHM_FORMAT_XRGB8888:
        return QImage::Format_RGB32;
    case WL_SHM_FORMAT_ARGB8888:
        return QImage::Format_ARGB32_Premultiplied;
    case WL_SHM_FORMAT_XBGR8888:
        return QImage::Format_RGBX8888;
    case WL_SHM_FORMAT_ABGR8888:
        return QImage::Format_RGBA8888_Premultiplied;
    default:
        return QImage::Format_Invalid;
    }
}

int formatRank(uint32_t fmt)
{
    switch (fmt) {
    case WL_SHM_FORMAT_XRGB8888: return 4;
    case WL_SHM_FORMAT_ARGB8888: return 3;
    case WL_SHM_FORMAT_XBGR8888: return 2;
    case WL_SHM_FORMAT_ABGR8888: return 1;
    default: return 0;
    }
}

struct Output {
    wl_output *output = nullptr;
    uint32_t globalName = 0;
    QString name;
    int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;

    // capture state
    enum State { Pending, Ready, Failed } state = Pending;
    QString error;
    ShmBuffer shm;
    bool yInvert = false;
    // wlr
    zwlr_screencopy_frame_v1 *wlrFrame = nullptr;
    int wlrW = 0, wlrH = 0, wlrStride = 0;
    uint32_t wlrFormat = 0;
    bool wlrHaveShm = false;
    // ext
    ext_image_capture_source_v1 *source = nullptr;
    ext_image_copy_capture_session_v1 *session = nullptr;
    ext_image_copy_capture_frame_v1 *frame = nullptr;
    int extW = 0, extH = 0;
    uint32_t extFormat = 0;
    int extFormatRank = -1;
    int32_t extTransform = WL_OUTPUT_TRANSFORM_NORMAL;
    bool extTransformKnown = false;

    ~Output()
    {
        if (wlrFrame)
            zwlr_screencopy_frame_v1_destroy(wlrFrame);
        if (frame)
            ext_image_copy_capture_frame_v1_destroy(frame);
        if (session)
            ext_image_copy_capture_session_v1_destroy(session);
        if (source)
            ext_image_capture_source_v1_destroy(source);
        if (output) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(output)) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
                wl_output_release(output);
            else
                wl_output_destroy(output);
        }
    }
};

class Connection {
public:
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    zwlr_screencopy_manager_v1 *wlr = nullptr;
    ext_output_image_capture_source_manager_v1 *extSources = nullptr;
    ext_image_copy_capture_manager_v1 *extCopy = nullptr;
    std::vector<std::unique_ptr<Output>> outputs;

    ~Connection()
    {
        outputs.clear();
        if (wlr)
            zwlr_screencopy_manager_v1_destroy(wlr);
        if (extCopy)
            ext_image_copy_capture_manager_v1_destroy(extCopy);
        if (extSources)
            ext_output_image_capture_source_manager_v1_destroy(extSources);
        if (shm)
            wl_shm_destroy(shm);
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
    }

    bool connect()
    {
        display = wl_display_connect(nullptr);
        if (!display)
            return false;
        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registryListener, this);
        wl_display_roundtrip(display);  // globals
        wl_display_roundtrip(display);  // output names / geometry
        return shm && !outputs.empty() && (wlr || (extSources && extCopy));
    }

    // Dispatch until every output left Pending, or the timeout expires.
    bool dispatchUntilDone(int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        auto pending = [this] {
            for (auto &o : outputs)
                if (o->state == Output::Pending)
                    return true;
            return false;
        };
        while (pending()) {
            while (wl_display_prepare_read(display) != 0)
                wl_display_dispatch_pending(display);
            wl_display_flush(display);
            const int left = timeoutMs - int(timer.elapsed());
            if (left <= 0) {
                wl_display_cancel_read(display);
                return false;
            }
            pollfd pfd{wl_display_get_fd(display), POLLIN, 0};
            const int r = poll(&pfd, 1, left);
            if (r <= 0) {
                wl_display_cancel_read(display);
                if (r < 0 && errno == EINTR)
                    continue;
                return false;
            }
            if (wl_display_read_events(display) < 0)
                return false;
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        }
        return true;
    }

    static void onGlobal(void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
    {
        auto *self = static_cast<Connection *>(data);
        if (strcmp(iface, wl_shm_interface.name) == 0) {
            self->shm = static_cast<wl_shm *>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        } else if (strcmp(iface, wl_output_interface.name) == 0) {
            auto out = std::make_unique<Output>();
            out->globalName = name;
            out->output = static_cast<wl_output *>(wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 4u)));
            wl_output_add_listener(out->output, &outputListener, out.get());
            self->outputs.push_back(std::move(out));
        } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
            self->wlr = static_cast<zwlr_screencopy_manager_v1 *>(
                wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u)));
        } else if (strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
            self->extSources = static_cast<ext_output_image_capture_source_manager_v1 *>(
                wl_registry_bind(reg, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        } else if (strcmp(iface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
            self->extCopy = static_cast<ext_image_copy_capture_manager_v1 *>(
                wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1));
        }
    }
    static void onGlobalRemove(void *, wl_registry *, uint32_t) {}
    static constexpr wl_registry_listener registryListener = {onGlobal, onGlobalRemove};

    static void outGeometry(void *data, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
                            const char *, const char *, int32_t transform)
    {
        static_cast<Output *>(data)->transform = transform;
    }
    static void outMode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
    static void outDone(void *, wl_output *) {}
    static void outScale(void *, wl_output *, int32_t) {}
    static void outName(void *data, wl_output *, const char *name)
    {
        static_cast<Output *>(data)->name = QString::fromUtf8(name);
    }
    static void outDescription(void *, wl_output *, const char *) {}
    static constexpr wl_output_listener outputListener = {outGeometry, outMode, outDone, outScale, outName, outDescription};
};

// ---- wlr-screencopy --------------------------------------------------------

struct WlrCtx {
    Connection *conn;
    Output *out;
};

void wlrStartCopy(Connection *conn, Output *o)
{
    if (!o->wlrHaveShm || qtFormat(o->wlrFormat) == QImage::Format_Invalid) {
        o->state = Output::Failed;
        o->error = QStringLiteral("no supported shm format (0x%1)").arg(o->wlrFormat, 0, 16);
        return;
    }
    if (!o->shm.create(conn->shm, o->wlrW, o->wlrH, o->wlrStride, o->wlrFormat)) {
        o->state = Output::Failed;
        o->error = QStringLiteral("failed to allocate shm buffer");
        return;
    }
    zwlr_screencopy_frame_v1_copy(o->wlrFrame, o->shm.buffer);
}

const zwlr_screencopy_frame_v1_listener wlrFrameListener = {
    // buffer
    [](void *data, zwlr_screencopy_frame_v1 *frame, uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
        auto *ctx = static_cast<WlrCtx *>(data);
        ctx->out->wlrFormat = format;
        ctx->out->wlrW = int(w);
        ctx->out->wlrH = int(h);
        ctx->out->wlrStride = int(stride);
        ctx->out->wlrHaveShm = true;
        // Version 1/2 have no buffer_done: copy right away.
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(frame)) < 3)
            wlrStartCopy(ctx->conn, ctx->out);
    },
    // flags
    [](void *data, zwlr_screencopy_frame_v1 *, uint32_t flags) {
        static_cast<WlrCtx *>(data)->out->yInvert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    },
    // ready
    [](void *data, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {
        static_cast<WlrCtx *>(data)->out->state = Output::Ready;
    },
    // failed
    [](void *data, zwlr_screencopy_frame_v1 *) {
        auto *o = static_cast<WlrCtx *>(data)->out;
        o->state = Output::Failed;
        o->error = QStringLiteral("compositor refused the screencopy");
    },
    // damage
    [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {},
    // linux_dmabuf
    [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {},
    // buffer_done
    [](void *data, zwlr_screencopy_frame_v1 *) {
        auto *ctx = static_cast<WlrCtx *>(data);
        wlrStartCopy(ctx->conn, ctx->out);
    },
};

// ---- ext-image-copy-capture ------------------------------------------------

const ext_image_copy_capture_frame_v1_listener extFrameListener = {
    // transform
    [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t transform) {
        auto *o = static_cast<Output *>(data);
        o->extTransform = int32_t(transform);
        o->extTransformKnown = true;
    },
    // damage
    [](void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {},
    // presentation_time
    [](void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {},
    // ready
    [](void *data, ext_image_copy_capture_frame_v1 *) { static_cast<Output *>(data)->state = Output::Ready; },
    // failed
    [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t reason) {
        auto *o = static_cast<Output *>(data);
        o->state = Output::Failed;
        o->error = QStringLiteral("ext-image-copy-capture failed (reason %1)").arg(reason);
    },
};

struct ExtCtx {
    Connection *conn;
    Output *out;
};

const ext_image_copy_capture_session_v1_listener extSessionListener = {
    // buffer_size
    [](void *data, ext_image_copy_capture_session_v1 *, uint32_t w, uint32_t h) {
        auto *o = static_cast<ExtCtx *>(data)->out;
        o->extW = int(w);
        o->extH = int(h);
    },
    // shm_format
    [](void *data, ext_image_copy_capture_session_v1 *, uint32_t format) {
        auto *o = static_cast<ExtCtx *>(data)->out;
        const int rank = formatRank(format);
        if (rank > o->extFormatRank) {
            o->extFormatRank = rank;
            o->extFormat = format;
        }
    },
    // dmabuf_device
    [](void *, ext_image_copy_capture_session_v1 *, wl_array *) {},
    // dmabuf_format
    [](void *, ext_image_copy_capture_session_v1 *, uint32_t, wl_array *) {},
    // done: constraints are known, capture one frame
    [](void *data, ext_image_copy_capture_session_v1 *session) {
        auto *ctx = static_cast<ExtCtx *>(data);
        Output *o = ctx->out;
        if (o->frame || o->state != Output::Pending)
            return;  // constraints re-sent; we only need one frame
        if (o->extFormatRank <= 0 || o->extW <= 0 || o->extH <= 0) {
            o->state = Output::Failed;
            o->error = QStringLiteral("no supported shm format");
            return;
        }
        if (!o->shm.create(ctx->conn->shm, o->extW, o->extH, o->extW * 4, o->extFormat)) {
            o->state = Output::Failed;
            o->error = QStringLiteral("failed to allocate shm buffer");
            return;
        }
        o->frame = ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(o->frame, &extFrameListener, o);
        ext_image_copy_capture_frame_v1_attach_buffer(o->frame, o->shm.buffer);
        ext_image_copy_capture_frame_v1_damage_buffer(o->frame, 0, 0, o->extW, o->extH);
        ext_image_copy_capture_frame_v1_capture(o->frame);
    },
    // stopped
    [](void *data, ext_image_copy_capture_session_v1 *) {
        auto *o = static_cast<ExtCtx *>(data)->out;
        if (o->state == Output::Pending) {
            o->state = Output::Failed;
            o->error = QStringLiteral("capture session stopped");
        }
    },
};

class WaylandCaptureBackend final : public CaptureBackend {
public:
    QString id() const override { return QStringLiteral("wayland"); }

    bool isAvailable() override
    {
        if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
            return false;
        Connection c;
        return c.connect();
    }

    CaptureResult grabAll(const QList<ScreenLayout> &) override
    {
        QElapsedTimer timer;
        timer.start();
        CaptureResult result;
        Connection conn;
        if (!conn.connect()) {
            result.error = QStringLiteral("compositor offers no screen capture protocol");
            return result;
        }
        const bool useExt = conn.extSources && conn.extCopy && !qEnvironmentVariableIsSet("SELSEARCH_FORCE_WLR_SCREENCOPY");
        result.backend = useExt ? QStringLiteral("ext-image-copy-capture-v1") : QStringLiteral("wlr-screencopy-v1");

        std::vector<std::unique_ptr<WlrCtx>> wlrCtx;
        std::vector<std::unique_ptr<ExtCtx>> extCtx;
        for (auto &o : conn.outputs) {
            if (useExt) {
                auto ctx = std::make_unique<ExtCtx>(ExtCtx{&conn, o.get()});
                o->source = ext_output_image_capture_source_manager_v1_create_source(conn.extSources, o->output);
                o->session = ext_image_copy_capture_manager_v1_create_session(conn.extCopy, o->source, 0);
                ext_image_copy_capture_session_v1_add_listener(o->session, &extSessionListener, ctx.get());
                extCtx.push_back(std::move(ctx));
            } else {
                auto ctx = std::make_unique<WlrCtx>(WlrCtx{&conn, o.get()});
                o->wlrFrame = zwlr_screencopy_manager_v1_capture_output(conn.wlr, 0, o->output);
                zwlr_screencopy_frame_v1_add_listener(o->wlrFrame, &wlrFrameListener, ctx.get());
                wlrCtx.push_back(std::move(ctx));
            }
        }
        if (!conn.dispatchUntilDone(3000)) {
            result.error = QStringLiteral("timed out waiting for the compositor");
            return result;
        }
        for (auto &o : conn.outputs) {
            if (o->state != Output::Ready) {
                qCWarning(lcCapture) << "output" << o->name << "failed:" << o->error;
                continue;
            }
            QImage raw(static_cast<const uchar *>(o->shm.data), o->shm.width, o->shm.height, o->shm.stride,
                       qtFormat(o->shm.format));
            // The ext frame reports the transform of its content; wlr-screencopy
            // buffers are in the output's untransformed (native) orientation.
            const int32_t transform = useExt ? (o->extTransformKnown ? o->extTransform : o->transform) : o->transform;
            QImage img = applyOutputTransform(raw, transform, o->yInvert);  // deep copy
            if (img.format() == QImage::Format_RGBX8888)
                img = img.convertToFormat(QImage::Format_RGB32);
            else if (img.format() == QImage::Format_RGBA8888_Premultiplied)
                img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            result.outputs.append(OutputGrab{o->name, std::move(img)});
        }
        if (result.outputs.isEmpty())
            result.error = QStringLiteral("no output could be captured");
        result.elapsedMs = timer.elapsed();
        return result;
    }
};

}  // namespace

std::unique_ptr<CaptureBackend> createWaylandCaptureBackend()
{
    return std::make_unique<WaylandCaptureBackend>();
}

}  // namespace selsearch
