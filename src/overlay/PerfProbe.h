// SPDX-License-Identifier: MIT
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>

class QQuickWindow;

namespace selsearch {

// Diagnostics for lens smoothness on machines we cannot test on. Enabled with
// SELSEARCH_PERF=1; once per second while a lens window is visible it logs
// (category selsearch.perf, always printed when enabled):
//   frames drawn, render-thread time per frame (avg/max), frame interval max,
//   pointer moves received, and the longest GUI-thread stall.
// The first frame also logs the graphics API, GPU, render loop and scale.
class PerfProbe : public QObject {
    Q_OBJECT
public:
    static bool enabled();
    PerfProbe(QQuickWindow *window, const QString &name, QObject *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void report();
    void describeOnce();

    QPointer<QQuickWindow> m_window;
    QString m_name;
    QTimer m_second;
    QTimer m_stallTick;
    QElapsedTimer m_stallClock;
    qint64 m_stallMax = 0;
    int m_moves = 0;
    bool m_described = false;
    // Written on the render thread.
    QElapsedTimer m_renderClock;
    QElapsedTimer m_swapClock;
    std::atomic<int> m_frames{0};
    std::atomic<qint64> m_renderNs{0};
    std::atomic<qint64> m_renderMaxNs{0};
    std::atomic<qint64> m_intervalMaxNs{0};
};

}  // namespace selsearch
