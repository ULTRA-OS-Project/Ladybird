/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free bridge for the downloads toolbar button (see Downloads.cpp for the LibWebView side).
// Mirrors the Bookmarks.h firewall pattern: plain std types only, includable from the X11-side
// BrowserWindow.cpp.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Ladybird {

// Number of currently-active downloads (in-progress or paused).
int active_download_count();

// X11-free mirror of WebView::FileDownloader::DownloadStatus, for the downloads popover.
enum class DownloadState {
    InProgress,
    Paused,
    Completed,
    Canceled,
    Failed,
};

// A snapshot of one download for the popover UI. `total`/`has_total` describe the size (total is
// meaningless when has_total is false); `can_resume` gates the Resume action on a paused download.
struct DownloadEntry {
    uint64_t id { 0 };
    std::string display_name;
    DownloadState state { DownloadState::InProgress };
    uint64_t downloaded { 0 };
    uint64_t total { 0 };
    bool has_total { false };
    bool can_resume { false };
};

// All downloads, most-recent first.
std::vector<DownloadEntry> download_entries();

// Per-download actions (no-ops for an unknown id).
void pause_download(uint64_t id);
void resume_download(uint64_t id);
void cancel_download(uint64_t id);
void open_download_file(uint64_t id);   // open the downloaded file with the default handler
void open_download_folder(uint64_t id); // reveal the file in the system file manager

// Register a callback fired whenever a download is added, updated or removed, so the chrome can
// refresh its downloads button/count. The backing observer is created lazily and lives for the
// process lifetime (see Downloads.cpp).
void set_on_downloads_changed(std::function<void()> callback);

}
