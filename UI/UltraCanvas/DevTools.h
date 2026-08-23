/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free bridge to the LibWebView DevTools/inspect actions, so the X11-side chrome
// (BrowserWindow.cpp) can drive them without including LibWebView. Implemented in
// DevTools.cpp on the LibWebView side of the firewall (see UltraCanvasPlatform.h).

#include <functional>
#include <string>

namespace Ladybird {

// Toggle the DevTools server on/off. Returns an error message, or an empty string on success.
std::string toggle_devtools();

// Launch the external DevTools client against the running server. Returns an error message,
// or an empty string on success.
std::string launch_devtools_client();

// The DevTools server port (0 when not configured/enabled).
int devtools_port();

// Inspect-menu actions targeting the active tab.
void view_source_active_tab();  // "View Source"
void open_task_manager();       // "Open Task Manager" (about:processes)

// Register a process-wide callback fired when the DevTools server is enabled/disabled, with the
// current enabled flag and port. Used by the chrome to show/hide the DevTools banner.
void set_on_devtools_state_changed(std::function<void(bool enabled, int port)>);

// Invoked by the LibWebView-side Application when the DevTools server toggles; fans out to the
// registered callback. Not called directly by the chrome.
void notify_devtools_state_changed(bool enabled, int port);

}
