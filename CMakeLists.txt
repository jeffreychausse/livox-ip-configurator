# ==============================================================================
# Livox IP Configurator - CMake Build Configuration
# ==============================================================================
#
# This CMake configuration builds a standalone Windows application for
# configuring Livox Mid-360 LiDAR sensor IP addresses.
#
# Tech Stack:
#   - C++17 with MSVC compiler
#   - Dear ImGui (Win32 + DirectX 11 backend)
#   - Livox SDK2 (statically linked)
#
# Build Instructions:
#   mkdir build && cd build
#   cmake .. -G "Visual Studio 17 2022" -A x64
#   cmake --build . --config Release
#
# The resulting executable will have no runtime dependencies (statically linked).
# ==============================================================================

cmake_minimum_required(VERSION 3.16)

# CMP0091: MSVC runtime library flags are selected by CMAKE_MSVC_RUNTIME_LIBRARY
# This must be set before the project() call to apply globally
cmake_policy(SET CMP0091 NEW)

project(livox-ip-configurator LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Platform Check - This project is Windows-only
# -----------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "This project only supports Windows.")
endif()

# -----------------------------------------------------------------------------
# C++ Standard
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Export compile commands for IDE support (VS Code IntelliSense)
# -----------------------------------------------------------------------------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Static Runtime Linking (/MT for Release, /MTd for Debug)
# Must be set before any targets are defined
# -----------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# -----------------------------------------------------------------------------
# Livox SDK2 (static library)
# -----------------------------------------------------------------------------
# BUILD_SHARED_LIBS=OFF ensures we get the static library variant
# EXCLUDE_FROM_ALL prevents SDK samples from being built
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

# The Livox SDK2 uses cmake_minimum_required(VERSION 3.0) which is deprecated
# in CMake 4.x. This policy setting allows it to configure anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(external/Livox-SDK2 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# Dear ImGui Library
# -----------------------------------------------------------------------------
# ImGui is compiled as a static library including the Win32 and DirectX 11
# backend implementations. This allows the main application to simply link
# against 'imgui' without managing individual source files.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)

add_library(imgui STATIC
    # Core ImGui source files
    ${IMGUI_DIR}/imgui.cpp          # Main ImGui implementation
    ${IMGUI_DIR}/imgui_draw.cpp     # Drawing primitives and command lists
    ${IMGUI_DIR}/imgui_tables.cpp   # Table/grid widget implementation
    ${IMGUI_DIR}/imgui_widgets.cpp  # Standard widgets (buttons, sliders, etc.)
    ${IMGUI_DIR}/imgui_demo.cpp     # Demo window (useful for development)
    
    # Platform and Renderer backend implementations
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp  # Win32 platform backend
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp   # DirectX 11 renderer backend
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Ensure ImGui also uses static runtime
set_target_properties(imgui PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# -----------------------------------------------------------------------------
# Main Application
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/Livox-SDK2/include
)

# Link against required libraries:
#   - imgui: Dear ImGui static library (defined above)
#   - livox_lidar_sdk_static: Livox SDK2 static library
#   - d3d11.lib: DirectX 11 API
#   - dxgi.lib: DirectX Graphics Infrastructure (swap chain, etc.)
#   - IPHLPAPI.lib: IP Helper API (network adapter enumeration)
#   - Ws2_32.lib: Windows Sockets 2 (required by IP Helper)
target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui
    livox_lidar_sdk_static
    d3d11.lib
    dxgi.lib
    IPHLPAPI.lib
    Ws2_32.lib
)

# Ensure main executable uses static runtime
set_target_properties(${PROJECT_NAME} PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# -----------------------------------------------------------------------------
# Output directory configuration
# -----------------------------------------------------------------------------
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
