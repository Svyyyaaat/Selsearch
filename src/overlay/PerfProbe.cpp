// SPDX-License-Identifier: MIT
#include "PerfProbe.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>

Q_LOGGING_CATEGORY(lcPerf, "selsearch.perf")

namespace selsearch {

namespace {
void atomicMax(std::atomic<qint64> &a, qint64 v)
{
    qint64 cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}
}  // namespace

bool PerfProbe::enabled()
{
    static const bool on = qEnvironmentVariableIntValue("SELSEARCH_PERF") != 0;
    return on;
}

PerfProbe::PerfProbe(QQuickWindow *window, const QString &name, QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_name(name)
{
    // Render thread: time from beforeRendering to frameSwapped, and the gap
    // between swaps (a long gap while the pointer moves is a visible hitch).
    connect(window, &QQuickWindow::beforeRendering, this, [this] { m_renderClock.start(); }, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_renderClock.isValid()) {
            const qint64 ns = m_renderClock.nsecsElapsed();
            m_renderNs += ns;
            atomicMax(m_renderMaxNs, ns);
        }
        if (m_swapClock.isValid())
            atomicMax(m_intervalMaxNs, m_swapClock.nsecsElapsed());
        m_swapClock.start();
        ++m_frames;
    }, Qt::DirectConnection);
    connect(window, &QQuickWindow::afterRendering, this, &PerfProbe::describeOnce, Qt::DirectConnection);
    window->installEventFilter(this);

    // GUI thread stall detector: a 4 ms timer that should never fire late.
    m_stallTick.setInterval(4);
    m_stallTick.setTimerType(Qt::PreciseTimer);
    connect(&m_stallTick, &QTimer::timeout, this, [this] {
        if (m_stallClock.isValid())
            m_stallMax = std::max(m_stallMax, m_stallClock.elapsed() - 4);
        m_stallClock.start();
    });
    m_stallTick.start();

    m_second.setInterval(1000);
    connect(&m_second, &QTimer::timeout, this, &PerfProbe::report);
    m_second.start();
}

bool PerfProbe::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove)
        ++m_moves;
    return false;
}

void PerfProbe::describeOnce()
{
    if (m_described || !m_window)
        return;
    m_described = true;
    QString api = QStringLiteral("unknown"), gpu;
    switch (m_window->rendererInterface()->graphicsApi()) {
    case QSGRendererInterface::OpenGL:
        api = QStringLiteral("OpenGL");
        if (QOpenGLContext *ctx = QOpenGLContext::currentContext()) {
            auto *f = ctx->functions();
            gpu = QString::fromLatin1(reinterpret_cast<const char *>(f->glGetString(GL_RENDERER))) + QStringLiteral(" / ")
                + QString::fromLatin1(reinterpret_cast<const char *>(f->glGetString(GL_VERSION)));
        }
        break;
    case QSGRendererInterface::Vulkan: api = QStringLiteral("Vulkan"); break;
    case QSGRendererInterface::Software: api = QStringLiteral("Software"); break;
    default: break;
    }
    QScreen *screen = m_window->screen();
    qCInfo(lcPerf).noquote() << QStringLiteral("%1: api=%2 gpu=\"%3\" loop=%4 qt=%5 size=%6x%7 dpr=%8 refresh=%9Hz platform=%10")
                                    .arg(m_name, api, gpu, qEnvironmentVariable("QSG_RENDER_LOOP", QStringLiteral("default")),
                                         QString::fromLatin1(qVersion()))
                                    .arg(m_window->width())
                                    .arg(m_window->height())
                                    .arg(m_window->devicePixelRatio())
                                    .arg(screen ? screen->refreshRate() : 0)
                                    .arg(QGuiApplication::platformName());
}

void PerfProbe::report()
{
    if (!m_window || !m_window->isVisible()) {
        m_frames = 0;
        m_renderNs = 0;
        m_renderMaxNs = 0;
        m_intervalMaxNs = 0;
        m_moves = 0;
        m_stallMax = 0;
        m_swapClock.invalidate();
        return;
    }
    const int frames = m_frames.exchange(0);
    const qint64 renderNs = m_renderNs.exchange(0);
    const qint64 renderMax = m_renderMaxNs.exchange(0);
    const qint64 intervalMax = m_intervalMaxNs.exchange(0);
    qCInfo(lcPerf).noquote() << QStringLiteral("%1: fps=%2 render avg=%3ms max=%4ms | longest frame gap=%5ms | pointer moves=%6 | ui stall max=%7ms")
                                    .arg(m_name)
                                    .arg(frames)
                                    .arg(frames ? renderNs / 1e6 / frames : 0.0, 0, 'f', 2)
                                    .arg(renderMax / 1e6, 0, 'f', 2)
                                    .arg(intervalMax / 1e6, 0, 'f', 1)
                                    .arg(m_moves)
                                    .arg(m_stallMax);
    m_moves = 0;
    m_stallMax = 0;
}

}  // namespace selsearch
