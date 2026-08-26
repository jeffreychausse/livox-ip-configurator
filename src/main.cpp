/**
 * @file main.cpp
 * @brief Livox IP Configurator - Main Application Entry Point
 * 
 * This application provides a graphical interface for configuring the IP address
 * of Livox Mid-360 LiDAR sensors. It uses Dear ImGui for the UI, rendered via
 * DirectX 11 on a Win32 window.
 * 
 * @note Windows headers have strict include ordering requirements:
 *       winsock2.h must be included before windows.h to avoid redefinition errors.
 */

// ============================================================================
// Windows Headers (order matters!)
// winsock2.h must come first to prevent winsock.h from being included via windows.h
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>       // Windows Sockets 2 API (must be before windows.h)
#include <ws2tcpip.h>       // TCP/IP utilities (inet_ntop, etc.)
#include <iphlpapi.h>       // IP Helper API for network adapter enumeration
#include <windows.h>        // Core Windows API

// ============================================================================
// DirectX 11 Headers
// ============================================================================
#include <d3d11.h>          // Direct3D 11 API

// ============================================================================
// Dear ImGui Headers
// ============================================================================
#include "imgui.h"              // Core ImGui API
#include "imgui_impl_win32.h"   // Win32 platform backend
#include "imgui_impl_dx11.h"    // DirectX 11 renderer backend

// ============================================================================
// Standard Library Headers
// ============================================================================
#include <vector>
#include <string>

// ============================================================================
// Forward declaration of Win32 message handler from imgui_impl_win32.cpp
// This is required as per ImGui documentation (see imgui_impl_win32.h line 34-36)
// ============================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// DirectX 11 Global State
// These are module-level variables managing the D3D11 rendering pipeline
// ============================================================================
static ID3D11Device*            g_pd3dDevice = nullptr;           // D3D11 device for resource creation
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;    // Immediate context for rendering commands
static IDXGISwapChain*          g_pSwapChain = nullptr;           // Swap chain for presenting frames
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr; // Render target for the back buffer
static bool                     g_SwapChainOccluded = false;      // True if window is minimized/occluded
static UINT                     g_ResizeWidth = 0;                // Pending resize width (0 = no resize)
static UINT                     g_ResizeHeight = 0;               // Pending resize height (0 = no resize)

// ============================================================================
// Forward Declarations - DirectX 11 Helper Functions
// ============================================================================
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// Network Adapter Data Structures
// ============================================================================

/**
 * @brief Represents a single network adapter with its basic properties
 */
struct NetworkAdapter {
    std::string name;           // Friendly name (e.g., "Ethernet", "Wi-Fi")
    std::string description;    // Hardware description (e.g., "Intel(R) Ethernet...")
    std::string ipAddress;      // IPv4 address in dotted-decimal notation
};

// List of discovered network adapters
static std::vector<NetworkAdapter> g_networkAdapters;

// Status message displayed in the UI
static std::string g_statusMessage = "Click 'Refresh Network Adapters' to scan.";

// ============================================================================
// Network Adapter Enumeration
// ============================================================================

/**
 * @brief Enumerates all active network adapters on the system
 * 
 * Uses the Windows IP Helper API (GetAdaptersAddresses) to discover network
 * interfaces. Filters out loopback adapters and non-operational interfaces.
 * Results are stored in g_networkAdapters and status is updated in g_statusMessage.
 */
void RefreshNetworkAdapters()
{
    // Clear previous results
    g_networkAdapters.clear();
    
    // First call to determine required buffer size
    ULONG bufferSize = 0;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;  // Include prefix information
    GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bufferSize);
    
    if (bufferSize == 0) {
        g_statusMessage = "Error: Failed to get adapter buffer size.";
        return;
    }
    
    // Allocate buffer and retrieve adapter information
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    
    DWORD result = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &bufferSize);
    if (result != NO_ERROR) {
        g_statusMessage = "Error: Failed to enumerate adapters (code: " + std::to_string(result) + ").";
        return;
    }
    
    // Iterate through all adapters
    for (PIP_ADAPTER_ADDRESSES pAdapter = pAddresses; pAdapter != nullptr; pAdapter = pAdapter->Next) {
        // Skip loopback interfaces (127.0.0.1)
        if (pAdapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        
        // Skip adapters that are not operational (disconnected, etc.)
        if (pAdapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        
        NetworkAdapter adapter;
        
        // Convert friendly name from wide string to UTF-8
        if (pAdapter->FriendlyName != nullptr) {
            int len = WideCharToMultiByte(CP_UTF8, 0, pAdapter->FriendlyName, -1, 
                                          nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                adapter.name.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, pAdapter->FriendlyName, -1,
                                    adapter.name.data(), len, nullptr, nullptr);
            }
        }
        
        // Convert description from wide string to UTF-8
        if (pAdapter->Description != nullptr) {
            int len = WideCharToMultiByte(CP_UTF8, 0, pAdapter->Description, -1,
                                          nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                adapter.description.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, pAdapter->Description, -1,
                                    adapter.description.data(), len, nullptr, nullptr);
            }
        }
        
        // Extract the first IPv4 unicast address
        for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pAdapter->FirstUnicastAddress;
             pUnicast != nullptr;
             pUnicast = pUnicast->Next) {
            
            // Only process IPv4 addresses
            if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                sockaddr_in* pSockAddr = reinterpret_cast<sockaddr_in*>(pUnicast->Address.lpSockaddr);
                char ipBuffer[INET_ADDRSTRLEN];
                
                // Convert binary IP to string representation
                if (inet_ntop(AF_INET, &(pSockAddr->sin_addr), ipBuffer, INET_ADDRSTRLEN) != nullptr) {
                    adapter.ipAddress = ipBuffer;
                }
                break;  // Only need the first IPv4 address
            }
        }
        
        g_networkAdapters.push_back(adapter);
    }
    
    // Update status message with results
    g_statusMessage = "Found " + std::to_string(g_networkAdapters.size()) + " active network adapter(s).";
}

// ============================================================================
// Application Entry Point
// ============================================================================

/**
 * @brief Windows GUI application entry point
 * 
 * Initializes the Win32 window, DirectX 11 rendering context, and Dear ImGui.
 * Runs the main application loop handling input, rendering, and cleanup.
 */
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    // Suppress unused parameter warnings
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    
    // Initialize Winsock (required for IP Helper API functions)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return 1;
    }
    
    // Enable DPI Awareness and get scale factor for primary monitor
    ImGui_ImplWin32_EnableDpiAwareness();
    POINT zeroPoint = { 0, 0 };
    HMONITOR hMonitor = MonitorFromPoint(zeroPoint, MONITOR_DEFAULTTOPRIMARY);
    float dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(hMonitor);
    
    // Create Win32 Window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"LivoxIPConfiguratorClass";
    
    if (!RegisterClassExW(&wc)) {
        WSACleanup();
        return 1;
    }
    
    // Create window with DPI-scaled dimensions
    int windowWidth = static_cast<int>(800 * dpiScale);
    int windowHeight = static_cast<int>(600 * dpiScale);
    
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"Livox IP Configurator", WS_OVERLAPPEDWINDOW,
        100, 100, windowWidth, windowHeight,
        nullptr, nullptr, wc.hInstance, nullptr);
    
    if (hwnd == nullptr) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        WSACleanup();
        return 1;
    }
    
    // Initialize DirectX 11
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        WSACleanup();
        return 1;
    }
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard nav
    
    // Configure visual style with DPI scaling
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;
    
    // Initialize platform and renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    
    // Background clear color (dark gray)
    const float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    
    // ========================================================================
    // Main Application Loop
    // ========================================================================
    bool running = true;
    while (running) {
        // Process Windows messages
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) break;
        
        // Skip rendering if window is occluded (minimized)
        if (g_SwapChainOccluded) {
            if (g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            g_SwapChainOccluded = false;
        }
        
        // Handle pending window resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                         DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }
        
        // Start new ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        // ====================================================================
        // Main UI Window
        // ====================================================================
        ImGui::Begin("Livox IP Configurator", nullptr, ImGuiWindowFlags_NoCollapse);
        
        ImGui::Text("Network Adapter Management");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Refresh network adapters button
        if (ImGui::Button("Refresh Network Adapters")) {
            RefreshNetworkAdapters();
        }
        
        ImGui::Spacing();
        ImGui::TextWrapped("Status: %s", g_statusMessage.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Display adapters in a table if any were found
        if (!g_networkAdapters.empty()) {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders
                                       | ImGuiTableFlags_RowBg
                                       | ImGuiTableFlags_Resizable;
            
            if (ImGui::BeginTable("NetworkAdaptersTable", 3, tableFlags)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("IP Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();
                
                for (const NetworkAdapter& adapter : g_networkAdapters) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(adapter.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(adapter.description.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(adapter.ipAddress.c_str());
                }
                ImGui::EndTable();
            }
        }
        
        ImGui::End();
        
        // ====================================================================
        // Render Frame
        // ====================================================================
        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        
        // Present with VSync enabled
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }
    
    // ========================================================================
    // Cleanup
    // ========================================================================
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    WSACleanup();
    
    return 0;
}

// ============================================================================
// DirectX 11 Device and Swap Chain Creation
// ============================================================================

/**
 * @brief Creates the D3D11 device, device context, and swap chain
 * 
 * Attempts to create a hardware-accelerated D3D11 device. Falls back to the
 * WARP software rasterizer if hardware acceleration is unavailable.
 * 
 * @param hWnd  Window handle for the swap chain's output window
 * @return      true if successful, false otherwise
 */
bool CreateDeviceD3D(HWND hWnd)
{
    // Configure swap chain
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;                             // Double buffering
    sd.BufferDesc.Width = 0;                        // Use window size
    sd.BufferDesc.Height = 0;                       // Use window size
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;                        // No MSAA
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    // Feature levels to attempt (prefer D3D 11.0, fall back to 10.0)
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;
    
    // Try hardware device first
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Hardware acceleration
        nullptr,                    // No software module
        0,                          // No flags
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);
    
    // Fall back to WARP (software) driver if hardware fails
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,   // Software rasterizer
            nullptr,
            0,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext);
    }
    
    if (FAILED(hr)) {
        return false;
    }
    
    CreateRenderTarget();
    return true;
}

/**
 * @brief Releases all D3D11 resources
 */
void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    
    if (g_pSwapChain != nullptr) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext != nullptr) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice != nullptr) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

/**
 * @brief Creates a render target view from the swap chain's back buffer
 */
void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    
    if (pBackBuffer != nullptr) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

/**
 * @brief Releases the render target view (called before swap chain resize)
 */
void CleanupRenderTarget()
{
    if (g_mainRenderTargetView != nullptr) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

// ============================================================================
// Win32 Window Procedure
// ============================================================================

/**
 * @brief Handles Win32 window messages
 * 
 * Forwards messages to ImGui for input processing, then handles window-specific
 * messages like resize, close, and system commands.
 */
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Let ImGui process the message first
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }
    
    switch (msg) {
    case WM_SIZE:
        // Queue resize (don't resize immediately to avoid issues during drag)
        if (wParam == SIZE_MINIMIZED) {
            return 0;
        }
        g_ResizeWidth = static_cast<UINT>(LOWORD(lParam));
        g_ResizeHeight = static_cast<UINT>(HIWORD(lParam));
        return 0;
        
    case WM_SYSCOMMAND:
        // Disable ALT key opening the system menu (interferes with ImGui)
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
        
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}