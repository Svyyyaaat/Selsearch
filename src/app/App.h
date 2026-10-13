// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QUrl>

#include <memory>

namespace selsearch {

class Config;
class FrameStore;
class IpcServer;
class LensController;
class OcrService;
class Translator;

class App : public QObject {
    Q_OBJECT
public:
    explicit App(QObject *parent = nullptr);
    ~App() override;

    bool init(QString *error);

    // Opens a URL produced by the UI. "selsearch:search?q=..." is resolved
    // with the configured engine; everything else opens as is.
    void openUrl(const QUrl &url);

private:
    void handle(const QJsonObject &request, const std::function<void(const QJsonObject &)> &reply);
    QJsonObject status() const;

    std::unique_ptr<Config> m_config;
    std::unique_ptr<IpcServer> m_ipc;
    std::unique_ptr<OcrService> m_ocr;
    std::unique_ptr<Translator> m_translator;
    std::unique_ptr<QQmlEngine> m_engine;
    FrameStore *m_frames = nullptr;  // owned by the engine
    std::unique_ptr<LensController> m_lens;
};

}  // namespace selsearch
