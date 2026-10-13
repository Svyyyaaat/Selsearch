// SPDX-License-Identifier: MIT
// Dev tool: grab every output with the selected backend and save PNGs.
//   selsearch-capture-dump OUTDIR [wayland|portal]
#include "capture/Capture.h"

#include <QCoreApplication>
#include <QDir>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    auto backend = selsearch::createCaptureBackend(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
    if (!backend) {
        std::fprintf(stderr, "no capture backend\n");
        return 1;
    }
    const auto r = backend->grabAll({});
    std::printf("backend=%s elapsed=%lldms error=%s\n", qPrintable(r.backend), r.elapsedMs, qPrintable(r.error));
    QDir().mkpath(out);
    for (const auto &o : r.outputs) {
        std::printf("%s %dx%d\n", qPrintable(o.name), o.image.width(), o.image.height());
        o.image.save(out + QLatin1Char('/') + o.name + QStringLiteral(".png"));
    }
    return r.ok() ? 0 : 1;
}
