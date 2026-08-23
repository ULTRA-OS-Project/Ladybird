/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the DevTools bridge (see DevTools.h): includes LibWebView (namespace GC)
// but never the X11-pulling UltraCanvas window/application headers.

#include <UI/UltraCanvas/DevTools.h>

#include <AK/String.h>
#include <LibURL/InternalURLs.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/Application.h>
#include <LibWebView/ViewImplementation.h>

namespace Ladybird {

static std::function<void(bool enabled, int port)> s_on_devtools_state_changed;

static std::string error_to_std_string(Error const& error)
{
    auto message = MUST(String::formatted("{}", error));
    auto view = message.bytes_as_string_view();
    return std::string { view.characters_without_null_termination(), view.length() };
}

std::string toggle_devtools()
{
    if (auto result = WebView::Application::the().toggle_devtools_enabled(); result.is_error())
        return error_to_std_string(result.error());
    return {};
}

std::string launch_devtools_client()
{
    if (auto result = WebView::Application::the().launch_devtools_client(); result.is_error())
        return error_to_std_string(result.error());
    return {};
}

int devtools_port()
{
    return WebView::Application::browser_options().devtools_port.value_or(0);
}

void view_source_active_tab()
{
    if (auto view = WebView::Application::the().active_web_view(); view.has_value())
        view->get_source();
}

void open_task_manager()
{
    WebView::Application::the().open_url_in_new_tab(URL::about_processes(), Web::HTML::ActivateTab::Yes);
}

void set_on_devtools_state_changed(std::function<void(bool enabled, int port)> callback)
{
    s_on_devtools_state_changed = move(callback);
}

void notify_devtools_state_changed(bool enabled, int port)
{
    if (s_on_devtools_state_changed)
        s_on_devtools_state_changed(enabled, port);
}

}
