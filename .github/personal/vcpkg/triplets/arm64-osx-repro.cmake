# SPDX-License-Identifier: MIT
# arm64-osx (static libraries), release only, with the same deployment target as the app.
# ZERO_AR_DATE makes Apple's ar/libtool write zero timestamps into static libraries.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_ENV_PASSTHROUGH ZERO_AR_DATE SOURCE_DATE_EPOCH)
