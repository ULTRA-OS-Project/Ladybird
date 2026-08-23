/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Ladybird {

// Installs a minimal Core::GeolocationProvider for the UltraCanvas frontend.
//
// The default platform provider on Linux is unimplemented, so navigator.geolocation
// requests already fail (with "position unavailable") rather than hang. This stub makes
// the outcome explicit — every request/watch is answered immediately with
// PermissionDenied — and marks the single extension point where a real provider (e.g. a
// GeoClue2/D-Bus source, mirroring UI/Qt/GeolocationProviderQt using Qt Positioning)
// should be plugged in.
void install_ultracanvas_geolocation_provider();

}
