// SPDX-License-Identifier: MIT
// Placeholder: the PP-OCRv5 ONNX engine is implemented separately.
#include "OcrEngine.h"

namespace selsearch {
std::unique_ptr<OcrEngine> createPaddleOcrEngine(const QString &) { return nullptr; }
}  // namespace selsearch
