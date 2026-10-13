// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

namespace selsearch {

// ~/.config/selsearch/config.toml, with built-in defaults for every key.
// Keys are dotted paths: "search.engine", "ai.model", ...
// The file only ever contains keys the user changed (or set in Settings).
class Config : public QObject {
    Q_OBJECT
public:
    explicit Config(QObject *parent = nullptr);
    ~Config() override;

    static QString defaultPath();
    // Load from `path` (defaultPath() when empty). Missing file = defaults.
    bool load(const QString &path = {});
    bool save() const;
    QString path() const { return m_path; }
    QString lastError() const { return m_error; }

    Q_INVOKABLE QVariant value(const QString &key) const;  // falls back to defaults()
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);  // saves immediately
    Q_INVOKABLE void reset(const QString &key);

    QString string(const QString &key) const { return value(key).toString(); }
    bool boolean(const QString &key) const { return value(key).toBool(); }
    int integer(const QString &key) const { return value(key).toInt(); }
    double number(const QString &key) const { return value(key).toDouble(); }
    QStringList list(const QString &key) const { return value(key).toStringList(); }

    static const QVariantMap &defaults();

Q_SIGNALS:
    void changed(const QString &key);
    void reloaded();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    QString m_path;
    QString m_error;
};

// The OCR / UI language list: config "ocr.languages", or derived from the
// system locale(s) plus English when empty.
QStringList effectiveOcrLanguages(const Config &config);

}  // namespace selsearch
