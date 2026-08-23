/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/UltraCanvas/GeolocationProvider.h>

#include <LibCore/GeolocationProvider.h>

namespace Ladybird {

// A no-op provider that denies every geolocation request. It exists so the outcome is a
// deliberate PermissionDenied rather than an incidental "unavailable" from the missing
// platform provider. Replace request_current_position/start_watching_position with a real
// source (GeoClue2 over D-Bus is the natural Linux choice) to make navigator.geolocation
// return actual coordinates.
class GeolocationProviderStub final : public Core::GeolocationProvider {
public:
    static ErrorOr<NonnullOwnPtr<GeolocationProvider>> create()
    {
        return adopt_nonnull_own_or_enomem(new (nothrow) GeolocationProviderStub);
    }

    virtual ErrorOr<RequestId, Core::GeolocationError> request_current_position(SuccessCallback, ErrorCallback) override
    {
        return denied();
    }

    virtual void cancel_current_position_request(RequestId) override { }

    virtual ErrorOr<WatchId, Core::GeolocationError> start_watching_position(SuccessCallback, ErrorCallback) override
    {
        return denied();
    }

    virtual void stop_watching_position(WatchId) override { }

private:
    static Core::GeolocationError denied()
    {
        return Core::GeolocationError {
            Core::GeolocationError::Type::PermissionDenied,
            "Geolocation is not available in this build"_string,
        };
    }
};

static ErrorOr<NonnullOwnPtr<Core::GeolocationProvider>> create_stub_provider()
{
    return GeolocationProviderStub::create();
}

static bool stub_provider_is_available()
{
    // A provider exists (it just denies), so report availability to keep the chrome's
    // permission flow consistent.
    return true;
}

void install_ultracanvas_geolocation_provider()
{
    Core::GeolocationProvider::set_provider_functions(create_stub_provider, stub_provider_is_available);
}

}
