/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the downloads bridge (see Downloads.h). This TU includes LibWebView
// (namespace GC) and so must stay X11-free — it never includes UltraCanvas window/app headers.

#include <LibCore/Process.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>

#include <UI/UltraCanvas/Downloads.h>

namespace Ladybird {

static std::function<void()> s_on_downloads_changed;

static WebView::FileDownloader& file_downloader()
{
    return WebView::Application::the().file_downloader();
}

int active_download_count()
{
    int count = 0;
    for (auto const& download : file_downloader().downloads()) {
        if (WebView::FileDownloader::status_is_active(download.status))
            ++count;
    }
    return count;
}

static DownloadState to_download_state(WebView::FileDownloader::DownloadStatus status)
{
    switch (status) {
    case WebView::FileDownloader::DownloadStatus::InProgress:
        return DownloadState::InProgress;
    case WebView::FileDownloader::DownloadStatus::Paused:
        return DownloadState::Paused;
    case WebView::FileDownloader::DownloadStatus::Completed:
        return DownloadState::Completed;
    case WebView::FileDownloader::DownloadStatus::Canceled:
        return DownloadState::Canceled;
    case WebView::FileDownloader::DownloadStatus::Failed:
        return DownloadState::Failed;
    }
    return DownloadState::Failed;
}

std::vector<DownloadEntry> download_entries()
{
    std::vector<DownloadEntry> entries;
    auto downloads = file_downloader().downloads();
    entries.reserve(downloads.size());
    // downloads() is in creation order; present most-recent first.
    for (size_t i = downloads.size(); i-- > 0;) {
        auto const& download = downloads[i];
        DownloadEntry entry;
        entry.id = download.id;
        auto name = download.destination.basename();
        entry.display_name = std::string { name.characters_without_null_termination(), name.length() };
        entry.state = to_download_state(download.status);
        entry.downloaded = download.downloaded_size;
        entry.has_total = download.total_size.has_value();
        entry.total = download.total_size.value_or(0);
        entry.can_resume = download.can_resume;
        entries.push_back(move(entry));
    }
    return entries;
}

void pause_download(uint64_t id)
{
    file_downloader().pause_download(id);
}

void resume_download(uint64_t id)
{
    file_downloader().resume_download(id);
}

void cancel_download(uint64_t id)
{
    file_downloader().cancel_download(id);
}

static void open_with_default_app(ByteString const& path)
{
    Vector<ByteString> arguments;
    arguments.append(path);
    Core::ProcessSpawnOptions options {
        .name = "xdg-open"sv,
        .executable = "xdg-open",
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };
    (void)Core::Process::spawn(options);
}

void open_download_file(uint64_t id)
{
    if (auto download = file_downloader().download(id); download.has_value())
        open_with_default_app(download->destination.string());
}

void open_download_folder(uint64_t id)
{
    if (auto download = file_downloader().download(id); download.has_value())
        open_with_default_app(ByteString { download->destination.dirname() });
}

namespace {

// Bridges the LibWebView download observer to the X11-side callback. The base ctor auto-registers.
class BridgeDownloadObserver final : public WebView::FileDownloaderObserver {
    virtual void download_added(WebView::FileDownloader::Download const&) override { notify(); }
    virtual void download_updated(WebView::FileDownloader::Download const&) override { notify(); }
    virtual void download_removed(u64) override { notify(); }

    static void notify()
    {
        if (s_on_downloads_changed)
            s_on_downloads_changed();
    }
};

// Intentionally leaked (raw new, never deleted): like the bookmark observers, this must live for
// the whole process and must NOT be torn down at static-destruction time — its base dtor calls
// remove_observer(), which dereferences the already-destroyed Application (use-after-free / SIGSEGV
// on exit). Leaking is harmless; the FileDownloader drops its observer ref when destroyed.
static BridgeDownloadObserver* s_download_observer = nullptr;

}

void set_on_downloads_changed(std::function<void()> callback)
{
    s_on_downloads_changed = move(callback);
    if (!s_download_observer)
        s_download_observer = new BridgeDownloadObserver;
}

}
