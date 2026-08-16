# A copy of the upstream directx-headers port, with one change: the pkg-config
# fixup runs with SKIP_CHECK.
#
# The check invokes pkg-config, which on a Windows host makes vcpkg download a
# pkgconf from the msys2 mirrors. msys2 only keeps its current packages, so the
# msys2-runtime that a pinned vcpkg asks for starts 404ing as soon as it is
# superseded, and the build dies while installing a header-only package. Nothing
# here consumes DirectX-Headers.pc -- the mod uses the CMake config -- so the
# .pc files are still rewritten, just not validated.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Microsoft/DirectX-Headers
    REF v${VERSION}
    SHA512 2098b307d5a8ce3f9b0830dbb4840242070f73ad6c51451d80f837cf13cc95dd17de83e8aafa9f9394ab04b8d23939afe95872c497e334d73d315693d5fc0c75
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS -DDXHEADERS_INSTALL=ON -DDXHEADERS_BUILD_TEST=OFF -DDXHEADERS_BUILD_GOOGLE_TEST=OFF
)

vcpkg_cmake_install()
vcpkg_fixup_pkgconfig(SKIP_CHECK)
vcpkg_cmake_config_fixup(CONFIG_PATH share/directx-headers/cmake)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
