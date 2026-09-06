/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free, LibWebView-free window-geometry persistence for the UltraCanvas chrome.
// Mirrors UI/Qt/Settings.{h,cpp} (which uses QSettings): the browser window saves its
// size/position/maximized state on move/resize/close and restores it on the next launch.
// Uses only AK + LibCore so it is safe to include on the X11 side (BrowserWindow.cpp).

#include <AK/Optional.h>

namespace Ladybird {

struct WindowGeometry {
    int x { -1 }; // -1 => let the window system position the window
    int y { -1 };
    int width { 950 };
    int height { 768 };
    bool maximized { false };
};

// Returns the persisted geometry, or an empty Optional when none is stored yet (or the
// file is missing/corrupt — a bad file is treated as "no saved geometry").
Optional<WindowGeometry> load_window_geometry();

// Best-effort persist; failures (unwritable config dir) are silently ignored.
void save_window_geometry(WindowGeometry const&);

}
