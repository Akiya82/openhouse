// openhouse's release version (the driver log, the telemetry hello, the overlay, the installer).
// Keep in sync with project(... VERSION) in CMakeLists.txt and vcpkg.json.
#pragma once

#define LH_VERSION_STRING "1.0.3"

namespace lh {
inline constexpr const char* kVersion = LH_VERSION_STRING;
// In the driver binary as is, so openhouse-installer can read which version is installed.
inline constexpr const char* kVersionMarker = "openhouse-version:" LH_VERSION_STRING ";";
}
