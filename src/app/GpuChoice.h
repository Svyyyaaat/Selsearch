// SPDX-License-Identifier: MIT
#pragma once

#include <QString>

namespace selsearch {

// Linux hybrid graphics (an iGPU plus an NVIDIA dGPU): which GPU renders the
// overlays. Must run before the QGuiApplication exists, because Qt picks the
// EGL implementation (GLVND vendor) when it connects to the display.
//
// general.gpu = "auto" (default) | "integrated" | "nvidia".
// "auto" renders on NVIDIA when the compositor's primary device
// (AQ_DRM_DEVICES / WLR_DRM_DEVICES) is the NVIDIA card or the NVIDIA card
// is awake anyway (e.g. it drives an external monitor). Rendering on the
// other GPU than the one that scans a monitor out means copying every frame
// across PCIe: measured on a Ryzen + RTX 3070 laptop, 9-17 ms per overlay
// frame on the iGPU vs 0.7 ms on the NVIDIA GPU, and 48 vs 10 ms UI stalls.
// When the dGPU is asleep, auto leaves it asleep (battery).
//
// Does nothing when the user already chose via the environment
// (__EGL_VENDOR_LIBRARY_FILENAMES, __NV_PRIME_RENDER_OFFLOAD, DRI_PRIME).
// Returns a one-line description of the decision for the log.
QString applyGpuChoice();
// The decision applyGpuChoice() made in this process ("" if not hybrid).
QString gpuDecision();

// Whether this machine has an NVIDIA GPU next to another one (to show the
// setting at all).
bool hasHybridNvidia();

}  // namespace selsearch
