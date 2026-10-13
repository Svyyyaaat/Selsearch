// SPDX-License-Identifier: MIT
// Placeholder: the streaming client is implemented separately.
#include "AiClient.h"

namespace selsearch {
AiClient::AiClient(QObject *parent) : QObject(parent) {}
bool AiClient::isLocalEndpoint() const { return true; }
void AiClient::ask(const QList<AiMessage> &) { Q_EMIT failed(QStringLiteral("not implemented")); }
void AiClient::abort() {}
void AiClient::onReadyRead() {}
void AiClient::onFinished() {}
}  // namespace selsearch
