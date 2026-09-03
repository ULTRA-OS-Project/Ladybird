/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Windows-only: the browser is built as a windowed (GUI-subsystem) application with no console, so
// stdout/stderr have nowhere to go by default. Redirect them to a log file up front so dbgln/warnln
// output and crash back-traces are still recorded. The log handle is opened inheritable and also
// installed as the process std handles, so the spawned helper services (WebContent, RequestServer,
// ...) that inherit them write into the same file. Path defaults to <exe_dir>\debug.log and can be
// overridden with `--debug-log <path>`; that argument is consumed here so the browser's own argument
// parser never sees it.

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>

#include <cstdio>
#include <cstring>

// AK/Windows.h pulls in winsock2.h + windows.h and must come after the AK/LibCore headers above.
#include <AK/Windows.h>
#include <fcntl.h>
#include <io.h>

namespace Ladybird {

void setup_windows_debug_log(Main::Arguments& arguments)
{
    // 1) Parse and strip --debug-log <path> / --debug-log=<path>.
    Optional<ByteString> log_path;
    Vector<char*> filtered_argv;
    if (arguments.argc > 0)
        filtered_argv.append(arguments.argv[0]);
    for (int i = 1; i < arguments.argc; ++i) {
        StringView arg { arguments.argv[i], strlen(arguments.argv[i]) };
        if (arg == "--debug-log"sv) {
            if (i + 1 < arguments.argc)
                log_path = ByteString { arguments.argv[++i] };
            continue;
        }
        if (arg.starts_with("--debug-log="sv)) {
            log_path = ByteString { arg.substring_view("--debug-log="sv.length()) };
            continue;
        }
        filtered_argv.append(arguments.argv[i]);
    }

    // 2) Default to a debug.log next to the executable.
    if (!log_path.has_value()) {
        if (auto exe = Core::System::current_executable_path(); !exe.is_error())
            log_path = LexicalPath { exe.value() }.parent().append("debug.log"sv).string();
        else
            log_path = ByteString { "debug.log" };
    }

    // 3) Point stderr (AK's dbgln/warnln write there via _write(_fileno(stderr), ...)) at the log.
    //    This is a GUI-subsystem (no console) build, so stderr has NO valid fd to _dup2 onto —
    //    _fileno(stderr) is -2. freopen() reopens the FILE* itself onto the file, giving it a real
    //    fd, which is the only thing that works here. Then mark the underlying handle inheritable and
    //    install it as the process std handles so spawned services (WebContent, ...) log here too.
    if (std::freopen(log_path->characters(), "w", stderr) != nullptr) {
        std::setvbuf(stderr, nullptr, _IONBF, 0); // unbuffered: the last lines before a crash reach disk

        auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
        if (handle != INVALID_HANDLE_VALUE) {
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetStdHandle(STD_ERROR_HANDLE, handle);
            SetStdHandle(STD_OUTPUT_HANDLE, handle);
        }
    }

    // 4) Publish the argument list without --debug-log. Leaked intentionally: it must outlive this
    //    call for the whole process.
    auto** argv_copy = new char*[filtered_argv.size() + 1];
    for (size_t i = 0; i < filtered_argv.size(); ++i)
        argv_copy[i] = filtered_argv[i];
    argv_copy[filtered_argv.size()] = nullptr;

    static Vector<StringView> s_filtered_strings;
    s_filtered_strings.clear_with_capacity();
    for (size_t i = 0; i < filtered_argv.size(); ++i)
        s_filtered_strings.append({ argv_copy[i], strlen(argv_copy[i]) });

    arguments.argc = static_cast<int>(filtered_argv.size());
    arguments.argv = argv_copy;
    arguments.strings = s_filtered_strings.span();
}

}
