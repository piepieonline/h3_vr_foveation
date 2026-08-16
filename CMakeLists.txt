cmake_minimum_required(VERSION 3.15)

project(VRFoveationFix CXX)

# Find latest version at https://github.com/OrfeasZ/ZHMModSDK/releases
# Set ZHMMODSDK_DIR variable to a local directory to use a local copy of the ZHMModSDK.
set(ZHMMODSDK_VER "v4.0.0-rc.3")
include(cmake/setup-zhmmodsdk.cmake)

# Set C++ standard to C++23.
set(CMAKE_CXX_STANDARD 23)

# Create the VRFoveationFix mod library.
add_library(VRFoveationFix SHARED
    src/VRFoveationFix.cpp
    src/VRFoveationFix.h
    src/CodePatcher.cpp
    src/CodePatcher.h
    src/ProcessInfo.cpp
    src/ProcessInfo.h
    src/RefractionFix.cpp
    src/RefractionFix.h
    src/MsvcIntrinsics.cpp
    src/Layout.h
)

# Set UTF-8 flag.
set_target_properties(VRFoveationFix PROPERTIES COMPILE_FLAGS "/utf-8")

# Add SDK and DX headers dependencies.
find_package(directx-headers CONFIG REQUIRED)

# The refraction split detours two game functions. The SDK's own hooks are a fixed, exported set,
# so this mod carries its own copy of the same library the SDK uses.
find_package(minhook CONFIG REQUIRED)

target_link_libraries(VRFoveationFix
    ZHMModSDK
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    implot
    minhook::minhook
)

install(TARGETS VRFoveationFix
    RUNTIME DESTINATION bin
)

# Install the mod to the game folder when the `GAME_INSTALL_PATH` variable is set.
zhmmodsdk_install(VRFoveationFix)
