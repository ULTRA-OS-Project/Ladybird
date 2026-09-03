# UltraCanvas GUI backend for Ladybird.
#
# Selected with -DLADYBIRD_GUI_FRAMEWORK=UltraCanvas (or `./Meta/ladybird.py ...
# --gui UltraCanvas`). This replaces the vendor's demo-app build that shipped at
# this path; the original is preserved alongside as CMakeLists.vendor-demoapp.txt.
#
# The uploaded UltraCanvas toolkit lives in the nested UltraCanvas/ subdirectory
# (UI/UltraCanvas/UltraCanvas/) and is compiled in-tree as a static library whose
# CMake target is `UltraCanvas`.

# --- Neutralize Ladybird's global compile flags for the UltraCanvas subtree ---
# The top-level build applies -fno-exceptions and -Werror -Wall -Wextra to every
# target (Meta/CMake/compile_options.cmake). UltraCanvas relies on C++ exceptions
# and does not build cleanly under -Werror. These directory-scoped options are
# appended AFTER the inherited ones; for conflicting flags the last one wins, so
# -fexceptions overrides -fno-exceptions and -Wno-error/-w defuse -Werror. They
# apply both to the nested UltraCanvas library and to our glue sources below,
# which include UltraCanvas headers that use exceptions and RTTI.
if (MSVC)
    # clang-cl: the global build disables exceptions with /EHs- (Meta/CMake/compile_options.cmake).
    # Re-enable exceptions (/EHsc) and RTTI (/GR) the MSVC way; -fexceptions is not a clang-cl flag.
    # Appended after the inherited flags, so last-one-wins re-enables exceptions. /w /WX- silence
    # UltraCanvas's non-clean warnings without failing the build.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/EHsc>
        $<$<COMPILE_LANGUAGE:CXX>:/GR>
        $<$<COMPILE_LANGUAGE:C,CXX>:/w>
        $<$<COMPILE_LANGUAGE:C,CXX>:/WX->
    )
else()
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:-fexceptions>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error>
        $<$<COMPILE_LANGUAGE:C,CXX>:-w>
    )
endif()

# --- Trim the UltraCanvas build to what the browser chrome needs ---
# Disable every optional plugin plus networking, audio, video, GL surfaces and the
# database module: the browser UI uses none of them, and turning them off keeps the
# dependency surface and build time down (and sidesteps the cargo/tesseract/MuPDF/microtex
# toolchains those plugins pull in).
#
# ULTRACANVAS_ENABLE_DATABASE is especially important to turn OFF: its UltraDatabase module
# links the *system* libsqlite3, which collides with Ladybird's vcpkg libsqlite3 in the final
# executable. With two sqlite libraries loaded, LibDatabase runs against the wrong one and every
# prepared statement fails at startup with "SQL logic error" (only reproduces here, not in Qt).
set(ULTRACANVAS_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_GL      OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_AUDIO   OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_VIDEO   OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_NET     OFF CACHE BOOL "" FORCE)
set(ULTRACANVAS_ENABLE_DATABASE OFF CACHE BOOL "" FORCE)
# libvips is kept enabled on every platform: it is UltraCanvas's raster image loader (PNG/JPEG/WebP/
# ...), so without it the browser chrome can't load images. On Windows it is supplied MSVC-ABI by
# vcpkg via libvips[cplusplus] (see vcpkg.json), which emits the vips + vips-cpp .pc files UC needs.
foreach(_uc_plugin
        CDR VECTOR XAR SVG PDF OCR VECTORIZER LATEX
        SMTP IMAP POP3 JMAP WEBDAV LDAP TELNET RTSP MQTT AMQP COAP
        SNMP SSH MDNS RTMP GRPC SIP RTP)
    set(ULTRACANVAS_PLUGIN_${_uc_plugin} OFF CACHE BOOL "" FORCE)
endforeach()

add_subdirectory(UltraCanvasSources/UltraCanvas)

# --- Product version, from the first line of the port's own Docs/CHANGELOG.md ---
# Format: `#### YYYY-MM-DD *x.y.z*`. Parsed here rather than via the vendor's
# UltraCanvasVersion.cmake so the port's version lives with the port and not in the
# UltraCanvasSources submodule. Keep the number out of the source: releasing is one new
# changelog line, which both LADYBIRD_VERSION and LADYBIRD_VERSION_DATE (the splash) follow.
set(_ladybird_changelog "${CMAKE_CURRENT_SOURCE_DIR}/Docs/CHANGELOG.md")
file(STRINGS "${_ladybird_changelog}" _ladybird_first_line LIMIT_COUNT 1)
string(REGEX MATCH "^#### ([0-9]+-[0-9]+-[0-9]+) \\*([0-9]+(\\.[0-9]+)*)\\*" _m "${_ladybird_first_line}")
if(NOT _m)
    message(FATAL_ERROR "Ladybird version: could not parse '#### YYYY-MM-DD *x.y.z*' from\n  ${_ladybird_changelog}\n  got: ${_ladybird_first_line}")
endif()
set(LADYBIRD_VERSION "${CMAKE_MATCH_2}")
set(LADYBIRD_VERSION_DATE "${CMAKE_MATCH_1}")
# Re-run configure when a release is added at the top of the changelog.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ladybird_changelog}")

# --- The ladybird executable ---
add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    Bookmarks.cpp
    DevTools.cpp
    Downloads.cpp
    GeolocationProvider.cpp
    Settings.cpp
    EventLoopImplementationUltraCanvas.cpp
    UltraCanvasPlatform.cpp
    WebContentView.cpp
    BrowserWindow.cpp
)
target_link_libraries(ladybird PRIVATE UltraCanvas)

target_compile_definitions(ladybird PRIVATE
    LADYBIRD_VERSION="${LADYBIRD_VERSION}"
    LADYBIRD_VERSION_DATE="${LADYBIRD_VERSION_DATE}")

create_ladybird_bundle(ladybird)

if (WIN32)
    # Redirects stdout/stderr (debug + crash logs) to a file since the windowed build has no console.
    target_sources(ladybird PRIVATE WindowsDebugLog.cpp)

    # Build as a windowed (GUI-subsystem) app so no console window appears. ladybird_windows_bin
    # (without CONSOLE) applies /ENTRY:mainCRTStartup, so the CRT still uses LibMain's main() rather
    # than requiring WinMain, and copies the vcpkg runtime DLLs next to the executable.
    ladybird_windows_bin(ladybird)

    # UltraCanvas registers its default UI fonts (Ubuntu) from <exe_dir>/Resources/media/fonts/ on
    # startup (GetResourcesDir() + GetBundledFontsDir() -> LoadBundledFontsNative). Stage them there
    # so the chrome renders with Ubuntu instead of falling back to a monospace default. These fonts
    # are what the other UltraCanvas apps ship, from the toolkit's media/fonts/.
    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/UltraCanvasSources/media/fonts"
            "$<TARGET_FILE_DIR:ladybird>/Resources/media/fonts"
        VERBATIM
    )

    # The splash / app-icon assets, resolved at runtime via GetResourcesDir() (= <exe_dir>/Resources/
    # on Windows). The Ladybird mark is port-owned (res/appicon); the UltraCanvas credit logo is the
    # shared framework asset from the vendor tree. Both are staged under media/ so the runtime paths
    # (media/appicon, media/images) are the same on every platform.
    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/appicon"
            "$<TARGET_FILE_DIR:ladybird>/Resources/media/appicon"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/UltraCanvasSources/media/images"
            "$<TARGET_FILE_DIR:ladybird>/Resources/media/images"
        VERBATIM
    )
elseif (UNIX AND NOT APPLE)
    # Stage the splash / app-icon assets where GetResourcesDir() looks on Linux: it picks the first
    # of <exe_dir>/share, <exe_dir>/../share, <exe_dir>/../share/UltraCanvas that contains media/,
    # so copying them under <exe_dir>/share/media/ makes candidate 1 resolve to <exe_dir>/share/.
    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/appicon"
            "$<TARGET_FILE_DIR:ladybird>/share/media/appicon"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/UltraCanvasSources/media/images"
            "$<TARGET_FILE_DIR:ladybird>/share/media/images"
        VERBATIM
    )
endif()
