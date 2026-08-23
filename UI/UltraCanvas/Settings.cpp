/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/UltraCanvas/Settings.h>

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>

namespace Ladybird {

static ByteString geometry_file_path()
{
    return LexicalPath::join(Core::StandardPaths::config_directory(), "UltraCanvasWindow.json"sv).string();
}

Optional<WindowGeometry> load_window_geometry()
{
    auto file = Core::File::open(geometry_file_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto json = JsonValue::from_string(contents.value());
    if (json.is_error() || !json.value().is_object())
        return {};
    auto const& object = json.value().as_object();

    WindowGeometry geometry;
    geometry.x = object.get_integer<int>("x"sv).value_or(geometry.x);
    geometry.y = object.get_integer<int>("y"sv).value_or(geometry.y);
    geometry.width = object.get_integer<int>("width"sv).value_or(geometry.width);
    geometry.height = object.get_integer<int>("height"sv).value_or(geometry.height);
    geometry.maximized = object.get_bool("maximized"sv).value_or(geometry.maximized);

    // Guard against a corrupt file yielding a degenerate window.
    if (geometry.width < 1 || geometry.height < 1)
        return {};
    return geometry;
}

void save_window_geometry(WindowGeometry const& geometry)
{
    JsonObject object;
    object.set("x"sv, geometry.x);
    object.set("y"sv, geometry.y);
    object.set("width"sv, geometry.width);
    object.set("height"sv, geometry.height);
    object.set("maximized"sv, geometry.maximized);

    auto file = Core::File::open(geometry_file_path(), Core::File::OpenMode::Write);
    if (file.is_error())
        return;
    (void)file.value()->write_until_depleted(object.serialized());
}

}
