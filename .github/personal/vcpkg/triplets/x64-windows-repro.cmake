# SPDX-License-Identifier: MIT
# x64-windows (DLLs), release only, with the pinned MSVC toolset and /Brepro so that the
# compiler and linker don't write timestamps into objects and PE files.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_PLATFORM_TOOLSET v143)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.44)
set(VCPKG_C_FLAGS "/Brepro")
set(VCPKG_CXX_FLAGS "/Brepro")
set(VCPKG_LINKER_FLAGS "/Brepro")
set(VCPKG_ENV_PASSTHROUGH SOURCE_DATE_EPOCH)
