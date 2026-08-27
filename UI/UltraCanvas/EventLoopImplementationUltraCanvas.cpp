/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// AK / LibCore first, so their headers are fully parsed before the UltraCanvas
// headers below pull in <X11/Xlib.h> (whose None/Bool/Status/... macros clash).
#include <AK/Vector.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/ThreadEventQueue.h>

#include <UI/UltraCanvas/EventLoopImplementationUltraCanvas.h>

#include <AK/Platform.h>
#include <pthread.h>
#include <signal.h>
#ifndef AK_OS_WINDOWS
#    include <fcntl.h>
#    include <unistd.h>
#endif
#ifdef AK_OS_WINDOWS
#    include <atomic>
#    include <memory>
#    include <mutex>
#    include <thread>
#endif

// UltraCanvas headers last (they include X11).
#include <UltraCanvasApplication.h>

namespace Ladybird {

// ===== SIGNALS =====
// Async-signal-safe self-pipe: the OS handler writes the signal number to the
// pipe; an fd-watch on the read end dispatches to registered handlers on the UI
// thread. Mirrors the Qt backend's SignalHandlers, but delivered through the
// UltraCanvas fd-watch instead of a QSocketNotifier.
namespace {

struct SignalRegistration {
    int signal_number { 0 };
    int id { 0 };
    Function<void(int)> handler;
};

HashMap<int, Vector<SignalRegistration>> s_signal_handlers;
int s_next_signal_id = 1;

void dispatch_signal(int signal_number)
{
    auto it = s_signal_handlers.find(signal_number);
    if (it == s_signal_handlers.end())
        return;
    // Iterate in place: the handler Function is move-only so the vector cannot be
    // copied. Handlers unregistering themselves mid-dispatch is not expected here.
    for (auto& registration : it->value)
        registration.handler(signal_number);
}

#ifdef AK_OS_WINDOWS
// Windows (MSVC/clang-cl) has no sigaction/self-pipe. Use the C signal() handler, which Windows
// delivers on a dedicated thread, and marshal the dispatch onto the UI thread via UltraCanvas.
EventLoopManagerUltraCanvas* s_signal_manager = nullptr;

void os_signal_handler(int signal_number)
{
    ::signal(signal_number, os_signal_handler); // Windows resets to SIG_DFL after each delivery.
    if (s_signal_manager)
        s_signal_manager->app().PostToUIThread([signal_number] { dispatch_signal(signal_number); });
}
#else
int s_signal_pipe[2] = { -1, -1 };

void os_signal_handler(int signal_number)
{
    // Runs in signal context: only async-signal-safe work (a single write()).
    if (s_signal_pipe[1] >= 0) {
        auto byte = static_cast<unsigned char>(signal_number);
        [[maybe_unused]] auto written = ::write(s_signal_pipe[1], &byte, 1);
    }
}
#endif

}

EventLoopManagerUltraCanvas::EventLoopManagerUltraCanvas(UltraCanvas::UltraCanvasApplicationBase& app)
    : m_app(app)
{
}

EventLoopManagerUltraCanvas::~EventLoopManagerUltraCanvas() = default;

NonnullOwnPtr<Core::EventLoopImplementation> EventLoopManagerUltraCanvas::make_implementation()
{
    return EventLoopImplementationUltraCanvas::create(*this);
}

intptr_t EventLoopManagerUltraCanvas::register_timer(Core::EventReceiver& object, int milliseconds, bool should_reload)
{
    auto timer_id = m_app.StartTimer(static_cast<unsigned int>(milliseconds), should_reload,
        [weak_object = object.make_weak_ptr()](UltraCanvas::TimerId) {
            auto object = weak_object.strong_ref();
            if (!object)
                return;
            Core::TimerEvent event;
            object->dispatch_event(event);
        });
    return static_cast<intptr_t>(timer_id);
}

void EventLoopManagerUltraCanvas::unregister_timer(intptr_t timer_id)
{
    m_app.StopTimer(static_cast<UltraCanvas::TimerId>(timer_id));
}

void EventLoopManagerUltraCanvas::register_notifier(Core::Notifier& notifier)
{
    auto type = notifier.type() == Core::Notifier::Type::Write
        ? UltraCanvas::FdWatchType::Write
        : UltraCanvas::FdWatchType::Read;

    auto watch_id = m_app.AddFdWatch(notifier.fd(), type,
        [weak_notifier = notifier.make_weak_ptr()]() {
            if (!weak_notifier)
                return;
            Core::NotifierActivationEvent event;
            static_cast<Core::Notifier&>(*weak_notifier).dispatch_event(event);
        });

    m_notifier_watch_ids.set(&notifier, watch_id);
    notifier.set_owner_thread(pthread_self());
}

void EventLoopManagerUltraCanvas::unregister_notifier(Core::Notifier& notifier)
{
    if (auto watch_id = m_notifier_watch_ids.take(&notifier); watch_id.has_value())
        m_app.RemoveFdWatch(*watch_id);
}

void EventLoopManagerUltraCanvas::did_post_event()
{
    // A Core event was posted (possibly from another thread): wake the UltraCanvas
    // loop and drain the thread event queue on the UI thread.
    m_app.PostToUIThread([] {
        Core::ThreadEventQueue::current().process();
    });
}

#ifdef AK_OS_WINDOWS
int EventLoopManagerUltraCanvas::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);
    s_signal_manager = this;

    int id = s_next_signal_id++;
    auto& handlers = s_signal_handlers.ensure(signal_number);
    bool is_first_for_signal = handlers.is_empty();
    handlers.append(SignalRegistration { signal_number, id, move(handler) });

    if (is_first_for_signal)
        ::signal(signal_number, os_signal_handler);
    return id;
}

void EventLoopManagerUltraCanvas::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);
    for (auto& entry : s_signal_handlers) {
        auto& handlers = entry.value;
        auto size_before = handlers.size();
        handlers.remove_all_matching([&](auto& registration) { return registration.id == handler_id; });
        if (handlers.size() != size_before && handlers.is_empty())
            ::signal(entry.key, SIG_DFL);
    }
}
#else
int EventLoopManagerUltraCanvas::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);

    if (s_signal_pipe[0] < 0) {
        if (::pipe(s_signal_pipe) == 0) {
            for (int i = 0; i < 2; ++i) {
                auto flags = ::fcntl(s_signal_pipe[i], F_GETFL);
                ::fcntl(s_signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
                ::fcntl(s_signal_pipe[i], F_SETFD, FD_CLOEXEC);
            }
            m_app.AddFdWatch(s_signal_pipe[0], UltraCanvas::FdWatchType::Read, [] {
                unsigned char buffer[64];
                ssize_t nread;
                while ((nread = ::read(s_signal_pipe[0], buffer, sizeof(buffer))) > 0) {
                    for (ssize_t i = 0; i < nread; ++i)
                        dispatch_signal(buffer[i]);
                }
            });
        }
    }

    int id = s_next_signal_id++;
    auto& handlers = s_signal_handlers.ensure(signal_number);
    bool is_first_for_signal = handlers.is_empty();
    handlers.append(SignalRegistration { signal_number, id, move(handler) });

    if (is_first_for_signal) {
        struct sigaction action = {};
        action.sa_handler = os_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        ::sigaction(signal_number, &action, nullptr);
    }
    return id;
}

void EventLoopManagerUltraCanvas::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);
    for (auto& entry : s_signal_handlers) {
        auto& handlers = entry.value;
        auto size_before = handlers.size();
        handlers.remove_all_matching([&](auto& registration) { return registration.id == handler_id; });
        if (handlers.size() != size_before && handlers.is_empty()) {
            struct sigaction action = {};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            ::sigaction(entry.key, &action, nullptr);
        }
    }
}
#endif

#ifdef AK_OS_WINDOWS
// Ladybird monitors each spawned child (WebContent, RequestServer, ...) for exit via
// register_process. UltraCanvas has no HANDLE notifier, so we wait on the process HANDLE on a
// background thread and marshal the exit handler onto the UI thread (mirrors the Qt backend's
// QWinEventNotifier-based implementation). A shared control block owns the cancel event so
// unregister_process can wake the waiter without racing the thread that owns the HANDLE.
namespace {

struct WinProcessMonitor {
    HANDLE cancel_event { nullptr };
    std::atomic<bool> cancelled { false };
    ~WinProcessMonitor()
    {
        if (cancel_event)
            CloseHandle(cancel_event);
    }
};

std::mutex s_process_mutex;
HashMap<pid_t, std::shared_ptr<WinProcessMonitor>> s_process_monitors;

}

void EventLoopManagerUltraCanvas::register_process(pid_t pid, ESCAPING Function<void(pid_t)> exit_handler)
{
    HANDLE process_handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process_handle)
        return; // Can't monitor this pid; exit detection simply won't fire for it.

    auto monitor = std::make_shared<WinProcessMonitor>();
    monitor->cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    {
        std::lock_guard<std::mutex> lock(s_process_mutex);
        if (s_process_monitors.contains(pid)) {
            CloseHandle(process_handle);
            return;
        }
        s_process_monitors.set(pid, monitor);
    }

    // Function<> is move-only; wrap it so the (copyable) PostToUIThread task can carry it.
    auto handler = std::make_shared<Function<void(pid_t)>>(move(exit_handler));
    std::thread([this, pid, process_handle, monitor, handler]() {
        HANDLE waits[2] = { process_handle, monitor->cancel_event };
        DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        CloseHandle(process_handle);

        {
            std::lock_guard<std::mutex> lock(s_process_mutex);
            s_process_monitors.remove(pid);
        }

        // WAIT_OBJECT_0 = the child exited; WAIT_OBJECT_0 + 1 = unregister_process cancelled us.
        if (result == WAIT_OBJECT_0 && !monitor->cancelled.load())
            m_app.PostToUIThread([pid, handler]() { (*handler)(pid); });
    }).detach();
}

void EventLoopManagerUltraCanvas::unregister_process(pid_t pid)
{
    std::shared_ptr<WinProcessMonitor> monitor;
    {
        std::lock_guard<std::mutex> lock(s_process_mutex);
        if (auto existing = s_process_monitors.get(pid); existing.has_value())
            monitor = existing.value();
    }
    if (monitor) {
        // We hold a ref, so cancel_event stays valid for this call even if the waiter races us.
        monitor->cancelled.store(true);
        SetEvent(monitor->cancel_event);
    }
}
#endif

EventLoopImplementationUltraCanvas::EventLoopImplementationUltraCanvas(EventLoopManagerUltraCanvas& manager)
    : m_manager(manager)
{
}

EventLoopImplementationUltraCanvas::~EventLoopImplementationUltraCanvas() = default;

int EventLoopImplementationUltraCanvas::exec()
{
    if (m_main_loop) {
        // Hand control to the UltraCanvas loop. Run() returns when the last window
        // closes or Exit() is called (see quit()). IPC/timers are serviced inside
        // each iteration via the notifier/timer bridges above.
        m_manager.app().Run();
        return m_exit_code;
    }

    // Nested Core::EventLoop (e.g. a synchronous IPC wait): pump until quit().
    while (!m_exit_requested)
        pump(PumpMode::WaitForEvents);
    return m_exit_code;
}

size_t EventLoopImplementationUltraCanvas::pump(PumpMode)
{
    auto processed = Core::ThreadEventQueue::current().process();
    m_manager.app().RunOnce();
    processed += Core::ThreadEventQueue::current().process();
    return processed;
}

void EventLoopImplementationUltraCanvas::quit(int code)
{
    m_exit_code = code;
    m_exit_requested = true;
    if (m_main_loop)
        m_manager.app().Exit();
    // Wake a blocked select() so the loop notices the exit request promptly.
    m_manager.app().PostToUIThread([] { });
}

void EventLoopImplementationUltraCanvas::wake()
{
    m_manager.app().PostToUIThread([] { });
}

bool EventLoopImplementationUltraCanvas::was_exit_requested() const
{
    if (m_main_loop)
        return m_exit_requested || !m_manager.app().IsRunning();
    return m_exit_requested;
}

}
