# Livox IP Configurator

A Windows application for configuring the IP address of Livox Mid-360 LiDAR sensors.

## Prerequisites

### 1. Visual Studio 2022 with C++ Support

You need **ONE** of the following:

**Option A: Full Visual Studio 2022 (Recommended)**
- Download from: https://visualstudio.microsoft.com/downloads/
- During installation, select the **"Desktop development with C++"** workload

**Option B: Visual Studio 2022 Build Tools**
- Download from: https://visualstudio.microsoft.com/downloads/ (scroll to "Tools for Visual Studio")
- Run the installer and select **"Desktop development with C++"** workload
- This installs the MSVC compiler, Windows SDK, and required libraries

### 2. CMake 3.16+

- Download from: https://cmake.org/download/
- Get the **"Windows x64 Installer"** (.msi file)
- During installation, select **"Add CMake to the system PATH for all users"**

### 3. VS Code Extensions (if using VS Code)

- **C/C++** (ms-vscode.cpptools)
- **CMake Tools** (ms-vscode.cmake-tools)

## Building

### Option 1: Using Developer Command Prompt (Recommended)

1. Open **"Developer Command Prompt for VS 2022"** from the Start menu
2. Navigate to the project directory and run:

```batch
cd C:\Users\jeffreyc\Documents\livox-ip-configurator
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

The executable will be at `build\bin\Release\livox-ip-configurator.exe`

### Option 2: Using VS Code

1. Open the project folder in VS Code
2. Install the required extensions (C/C++ and CMake Tools)
3. Press `Ctrl+Shift+P` and type **"CMake: Select Configure Preset"**
4. Select **"Visual Studio 2022 (x64)"**
5. Press `Ctrl+Shift+P` and type **"CMake: Configure"**
6. Press `Ctrl+Shift+P` and type **"CMake: Build"**

## Fixing "Cannot open source file" Errors in VS Code

If you see errors like `cannot open source file "winsock2.h"`, follow these steps:

1. **Ensure CMake Tools extension is installed**
   - Press `Ctrl+Shift+X` to open Extensions
   - Search for "CMake Tools" and install it

2. **Configure the CMake project**
   - Press `Ctrl+Shift+P`
   - Type "CMake: Select a Kit" and press Enter
   - Select a Visual Studio 2022 kit (e.g., "Visual Studio Community 2022 Release - amd64")

3. **Run CMake Configure**
   - Press `Ctrl+Shift+P`
   - Type "CMake: Configure" and press Enter

4. **Reload the window**
   - Press `Ctrl+Shift+P`
   - Type "Developer: Reload Window" and press Enter

The IntelliSense errors should disappear after CMake successfully configures the project.

## Tech Stack

- C++17 with MSVC compiler
- Dear ImGui (Win32 + DirectX 11 backend)
- Livox SDK2 (statically linked)
- Static runtime linking (/MT) for no runtime dependencies
