// SPDX-License-Identifier: MIT
// Placeholder: translation backends are implemented separately.
#include "Translator.h"

namespace selsearch {
std::unique_ptr<Translator> createTranslator(const Config &, QObject *) { return nullptr; }
}  // namespace selsearch
