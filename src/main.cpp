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
// Livox SDK2 Headers
// ============================================================================
#include "livox_lidar_def.h"    // Livox SDK2 type/struct definitions
#include "livox_lidar_api.h"    // Livox SDK2 public API

// ============================================================================
// Standard Library Headers
// ============================================================================
#include <vector>
#include <string>
#include <mutex>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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

// Currently selected adapter index in the combo box (-1 = none selected)
static int g_selectedAdapterIndex = -1;

// ============================================================================
// Livox SDK2 Data Structures & State
// ============================================================================

/**
 * @brief Represents a single discovered Livox Mid-360 sensor
 */
struct LidarDeviceInfo {
    uint32_t    handle = 0;        // SDK device handle (encodes the lidar's IPv4 address)
    std::string serialNumber;      // Sensor serial number
    std::string currentIp;         // Sensor's current IP address
    uint8_t     devType = 0;       // Device type (see LivoxLidarDeviceType)
    bool        online = true;     // Liveness state derived from lastSeenTime
    std::chrono::steady_clock::time_point lastSeenTime;  // Updated on each discovery callback

    // Full IP configuration obtained via QueryLivoxLidarInternalInfo()
    bool        ipConfigQueried = false;  // true once the query has returned successfully
    std::string currentNetmask;           // Subnet mask (e.g. "255.255.255.0")
    std::string currentGateway;           // Gateway (e.g. "192.168.1.1")
};

// Discovered sensors, protected by g_lidarDevicesMutex since updates arrive on
// the Livox SDK's internal callback thread while the UI thread reads them.
static std::mutex g_lidarDevicesMutex;
static std::map<uint32_t, LidarDeviceInfo> g_lidarDevices;

// Handle of the sensor currently selected in the discovered sensors table (0 = none)
static uint32_t g_selectedLidarHandle = 0;

// True after a successful "Push New IP" until the sensor is rebooted or deselected
static bool g_rebootRequired = false;

// Whether the Livox SDK has been successfully initialized/started
static bool g_sdkInitialized = false;

// Path to the temporary JSON configuration file generated for LivoxLidarSdkInit()
static std::string g_tempConfigPath;

// Input buffers for the IP configuration panel
static char g_newIpBuffer[64]     = "";
static char g_netmaskBuffer[64]   = "";
static char g_gatewayBuffer[64]   = "";

// Rolling log of status/informational messages shown at the bottom of the UI,
// protected by g_logMutex since SDK callbacks append to it from other threads.
static std::mutex g_logMutex;
static std::vector<std::string> g_logMessages;

/**
 * @brief Appends a message to the on-screen log (thread-safe)
 */
void AddLogMessage(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logMessages.push_back(message);
    // Keep the log from growing unbounded
    constexpr size_t kMaxLogMessages = 500;
    if (g_logMessages.size() > kMaxLogMessages) {
        g_logMessages.erase(g_logMessages.begin(), g_logMessages.begin() + (g_logMessages.size() - kMaxLogMessages));
    }
    g_statusMessage = message;
}

/**
 * @brief Converts a Livox SDK device handle to its dotted-decimal IPv4 string.
 *
 * Livox SDK2 encodes the lidar's IPv4 address directly in the uint32_t handle
 * (see the various samples using `struct in_addr; addr.s_addr = handle;`).
 */
std::string HandleToIpString(uint32_t handle)
{
    struct in_addr addr;
    addr.s_addr = handle;
    char buffer[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &addr, buffer, INET_ADDRSTRLEN) != nullptr) {
        return std::string(buffer);
    }
    return "0.0.0.0";
}

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

    // Keep the adapter selection valid after a refresh
    if (g_selectedAdapterIndex >= static_cast<int>(g_networkAdapters.size())) {
        g_selectedAdapterIndex = g_networkAdapters.empty() ? -1 : 0;
    } else if (g_selectedAdapterIndex < 0 && !g_networkAdapters.empty()) {
        g_selectedAdapterIndex = 0;
    }
}

// ============================================================================
// Livox SDK2 Callbacks
// ============================================================================

/**
 * @brief Callback invoked whenever a Livox lidar comes online or its info changes.
 *
 * Registered via SetLivoxLidarInfoChangeCallback(). Runs on an SDK-internal
 * thread, so all shared state is updated under g_lidarDevicesMutex.
 */
void OnLivoxLidarInfoChange(const uint32_t handle, const LivoxLidarInfo* info, void* clientData)
{
    UNREFERENCED_PARAMETER(clientData);
    if (info == nullptr) {
        AddLogMessage("Lidar info change callback received null info.");
        return;
    }

    bool isNewDevice = false;
    {
        std::lock_guard<std::mutex> lock(g_lidarDevicesMutex);
        isNewDevice = (g_lidarDevices.find(handle) == g_lidarDevices.end());

        // Update discovery fields while preserving any previously queried IP config
        LidarDeviceInfo& device = g_lidarDevices[handle];
        device.handle = handle;
        device.serialNumber = info->sn;
        device.currentIp = HandleToIpString(handle);
        device.devType = info->dev_type;
        device.online = true;
        device.lastSeenTime = std::chrono::steady_clock::now();
        // ipConfigQueried, currentNetmask, currentGateway are left as-is for
        // existing entries so a previously successful query is not discarded.
    }

    if (isNewDevice) {
        const std::string sn = info->sn;
        AddLogMessage("Discovered sensor SN:" + sn + " at IP:" + HandleToIpString(handle));
    }
}

/**
 * @brief Callback invoked when a SetLivoxLidarIp() request completes.
 */
void OnSetLivoxLidarIp(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* response, void* clientData)
{
    UNREFERENCED_PARAMETER(clientData);
    if (response == nullptr) {
        AddLogMessage("Set IP failed: no response from sensor (handle:" + std::to_string(handle) + ").");
        return;
    }

    if (status == kLivoxLidarStatusSuccess && response->ret_code == 0 && response->error_key == 0) {
        g_rebootRequired = true;
        AddLogMessage("IP set successfully. Reboot required for the change to take effect.");
    } else {
        AddLogMessage("Error: Failed to set IP (status:" + std::to_string(status) +
                      " ret_code:" + std::to_string(response->ret_code) +
                      " error_key:" + std::to_string(response->error_key) + ").");
    }
}

/**
 * @brief Callback invoked when a LivoxLidarRequestReboot() request completes.
 */
void OnLivoxLidarReboot(livox_status status, uint32_t handle, LivoxLidarRebootResponse* response, void* clientData)
{
    UNREFERENCED_PARAMETER(clientData);
    UNREFERENCED_PARAMETER(handle);
    if (response == nullptr) {
        AddLogMessage("Reboot request failed: no response from sensor.");
        return;
    }

    if (status == kLivoxLidarStatusSuccess && response->ret_code == 0) {
        g_rebootRequired = false;
        AddLogMessage("Sensor is rebooting to apply the new configuration.");
    } else {
        AddLogMessage("Error: Reboot request failed (status:" + std::to_string(status) +
                      " ret_code:" + std::to_string(response->ret_code) + ").");
    }
}

/**
 * @brief Callback invoked when a QueryLivoxLidarInternalInfo() request completes.
 *
 * Parses the KV response to extract kKeyLidarIpCfg (key 0x0004) which contains
 * the sensor's current IP address, subnet mask, and gateway (12 bytes total).
 * Updates the cached LidarDeviceInfo and fills the UI input buffers so the user
 * sees the sensor's actual configuration when they click on it.
 *
 * Runs on an SDK-internal thread; shared state is updated under g_lidarDevicesMutex.
 */
void OnQueryLidarInternalInfo(livox_status status, uint32_t handle,
                              LivoxLidarDiagInternalInfoResponse* response, void* clientData)
{
    UNREFERENCED_PARAMETER(clientData);

    if (status != kLivoxLidarStatusSuccess) {
        AddLogMessage("Query IP config failed (status:" + std::to_string(status) +
                      " handle:" + std::to_string(handle) + ").");
        return;
    }
    if (response == nullptr) {
        AddLogMessage("Query IP config failed: null response (handle:" + std::to_string(handle) + ").");
        return;
    }
    if (response->ret_code != 0) {
        AddLogMessage("Query IP config failed (ret_code:" + std::to_string(response->ret_code) +
                      " handle:" + std::to_string(handle) + ").");
        return;
    }

    // Walk the packed KV list looking for kKeyLidarIpCfg (0x0004).
    // The value is 12 bytes: ip[4] + subnet_mask[4] + gateway[4].
    std::string queriedIp;
    std::string queriedNetmask;
    std::string queriedGateway;
    bool found = false;

    uint16_t off = 0;
    for (uint16_t i = 0; i < response->param_num; ++i) {
        if (off + sizeof(uint16_t) * 2 > 1400) break;  // safety bound
        LivoxLidarKeyValueParam* kv = reinterpret_cast<LivoxLidarKeyValueParam*>(&response->data[off]);

        if (kv->key == static_cast<uint16_t>(kKeyLidarIpCfg) && kv->length >= 12) {
            const uint8_t* v = &kv->value[0];
            queriedIp      = std::to_string(v[0]) + "." + std::to_string(v[1]) + "." +
                             std::to_string(v[2]) + "." + std::to_string(v[3]);
            queriedNetmask = std::to_string(v[4]) + "." + std::to_string(v[5]) + "." +
                             std::to_string(v[6]) + "." + std::to_string(v[7]);
            queriedGateway = std::to_string(v[8]) + "." + std::to_string(v[9]) + "." +
                             std::to_string(v[10]) + "." + std::to_string(v[11]);
            found = true;
            break;
        }

        // Advance past this KV entry: key(2) + length(2) + value(kv->length)
        off += sizeof(uint16_t) * 2 + kv->length;
    }

    if (!found) {
        AddLogMessage("Query IP config: kKeyLidarIpCfg not found in response (handle:" +
                      std::to_string(handle) + ").");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_lidarDevicesMutex);
        auto it = g_lidarDevices.find(handle);
        if (it != g_lidarDevices.end()) {
            it->second.ipConfigQueried = true;
            it->second.currentNetmask  = queriedNetmask;
            it->second.currentGateway  = queriedGateway;
            // Update the IP from the query too — it's the authoritative source
            it->second.currentIp = queriedIp;
        }

        // If this handle is still the selected sensor, populate the input buffers
        if (handle == g_selectedLidarHandle) {
            strncpy_s(g_newIpBuffer,   queriedIp.c_str(),      sizeof(g_newIpBuffer) - 1);
            strncpy_s(g_netmaskBuffer, queriedNetmask.c_str(), sizeof(g_netmaskBuffer) - 1);
            strncpy_s(g_gatewayBuffer, queriedGateway.c_str(), sizeof(g_gatewayBuffer) - 1);
        }
    }

    AddLogMessage("Queried IP config for handle:" + std::to_string(handle) +
                  " — IP:" + queriedIp + " Mask:" + queriedNetmask + " GW:" + queriedGateway);
}

// ============================================================================
// Livox SDK2 Initialization
// ============================================================================

/**
 * @brief Writes a minimal Livox SDK2 JSON configuration for MID360 auto-discovery
 *        to a temporary file and returns the file path.
 *
 * The SDK's LivoxLidarSdkInit() only accepts a path to a JSON file on disk, so
 * the configuration is generated in memory as a string and then flushed to a
 * temp file that the SDK can parse.
 *
 * @param hostIp  The local network adapter IP address to bind to.
 * @return        Path to the generated config file, or an empty string on failure.
 */
std::string GenerateLivoxConfigFile(const std::string& hostIp)
{
    std::ostringstream json;
    json << "{\n"
         << "  \"MID360\": {\n"
         << "    \"lidar_net_info\": {\n"
         << "      \"cmd_data_port\": 56100,\n"
         << "      \"push_msg_port\": 56200,\n"
         << "      \"point_data_port\": 56300,\n"
         << "      \"imu_data_port\": 56400,\n"
         << "      \"log_data_port\": 56500\n"
         << "    },\n"
         << "    \"host_net_info\": [\n"
         << "      {\n"
         << "        \"host_ip\": \"" << hostIp << "\",\n"
         << "        \"multicast_ip\": \"\",\n"
         << "        \"cmd_data_port\": 56101,\n"
         << "        \"push_msg_port\": 56201,\n"
         << "        \"point_data_port\": 56301,\n"
         << "        \"imu_data_port\": 56401,\n"
         << "        \"log_data_port\": 56501\n"
         << "      }\n"
         << "    ]\n"
         << "  }\n"
         << "}\n";

    char tempDir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, tempDir) == 0) {
        AddLogMessage("Error: Failed to get a temp directory for the SDK config file.");
        return "";
    }

    char tempFilePath[MAX_PATH] = {};
    if (GetTempFileNameA(tempDir, "lvx", 0, tempFilePath) == 0) {
        AddLogMessage("Error: Failed to create a temp file for the SDK config.");
        return "";
    }

    std::ofstream outFile(tempFilePath, std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) {
        AddLogMessage("Error: Failed to write the SDK config file.");
        return "";
    }
    outFile << json.str();
    outFile.close();

    return std::string(tempFilePath);
}

/**
 * @brief Checks whether any of the Livox SDK UDP ports are already bound by
 *        another process and logs warnings for each conflict found.
 *
 * Uses the Windows IP Helper API (GetExtendedUdpTable) to enumerate all UDP
 * endpoints currently bound on the system together with their owning PIDs.
 * This works reliably even when SO_REUSEADDR is set (which the Livox SDK
 * uses on all of its sockets), unlike a test-bind approach that would
 * silently succeed in that case.
 *
 * Called before LivoxLidarSdkInit() so the user gets a clear, actionable
 * warning if another application (e.g. another SDK instance, ROS driver,
 * or Livox Viewer) already holds any of the required ports.
 */
static void CheckLivoxPortsInUse(const std::string& hostIp)
{
    UNREFERENCED_PARAMETER(hostIp);

    struct PortInfo { uint16_t port; const char* name; };
    static const PortInfo ports[] = {
        { 56000, "Detection"        },
        { 56101, "Host Cmd"         },
        { 56201, "Host Push Msg"    },
        { 56301, "Host Point Cloud" },
        { 56401, "Host IMU Data"    },
        { 56501, "Host Log Data"    },
    };

    // Query the OS for all UDP endpoints.  First call with size 0 to learn
    // the required buffer size, then allocate and call again.
    DWORD tableSize = 0;
    DWORD result = GetExtendedUdpTable(nullptr, &tableSize, FALSE, AF_INET,
                                       UDP_TABLE_OWNER_PID, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER) {
        return;  // Unexpected — can't enumerate
    }

    std::vector<uint8_t> buffer(tableSize);
    result = GetExtendedUdpTable(buffer.data(), &tableSize, FALSE, AF_INET,
                                 UDP_TABLE_OWNER_PID, 0);
    if (result != NO_ERROR) {
        return;
    }

    const MIB_UDPTABLE_OWNER_PID* udpTable =
        reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());

    DWORD ownPid = GetCurrentProcessId();

    for (const auto& p : ports) {
        uint16_t netPort = htons(p.port);
        for (DWORD i = 0; i < udpTable->dwNumEntries; ++i) {
            const MIB_UDPROW_OWNER_PID& row = udpTable->table[i];
            if (row.dwLocalPort == netPort && row.dwOwningPid != ownPid) {
                AddLogMessage("Warning: UDP port " + std::to_string(p.port) +
                              " (" + p.name + ") is already bound by PID " +
                              std::to_string(row.dwOwningPid) +
                              ". Another application may be using the Livox SDK.");
                break;  // One match per port is enough
            }
        }
    }
}

/**
 * @brief Initializes and starts the Livox SDK2 using the given host adapter IP.
 *
 * Generates an in-memory MID360 configuration, writes it to a temp file,
 * calls LivoxLidarSdkInit()/LivoxLidarSdkStart(), and registers the device
 * discovery callback.
 */
void StartLivoxSdk(const std::string& hostIp)
{
    if (g_sdkInitialized) {
        AddLogMessage("SDK is already running. Stop is not required between rescans.");
        return;
    }

    if (hostIp.empty()) {
        AddLogMessage("Error: Please select a network adapter with a valid IPv4 address first.");
        return;
    }

    g_tempConfigPath = GenerateLivoxConfigFile(hostIp);
    if (g_tempConfigPath.empty()) {
        return;
    }

    // Check for port conflicts before init — the SDK uses SO_REUSEADDR so
    // init will succeed even when ports are already held by another process,
    // but discovery will silently fail because responses go to the other app.
    CheckLivoxPortsInUse(hostIp);

    if (!LivoxLidarSdkInit(g_tempConfigPath.c_str())) {
        AddLogMessage("Error: LivoxLidarSdkInit() failed. Check the adapter IP and try again.");
        LivoxLidarSdkUninit();
        return;
    }

    SetLivoxLidarInfoChangeCallback(OnLivoxLidarInfoChange, nullptr);

    if (!LivoxLidarSdkStart()) {
        AddLogMessage("Error: LivoxLidarSdkStart() failed.");
        LivoxLidarSdkUninit();
        return;
    }

    g_sdkInitialized = true;
    AddLogMessage("Connected to SDK. Listening for Mid-360 sensors on " + hostIp + "...");
}

/**
 * @brief Stops and uninitializes the Livox SDK2, releasing all sockets/threads
 *        it holds so the user can pick a different network adapter.
 *
 * Clears the discovered sensor list and the current sensor selection, since
 * they belonged to the now-stopped SDK session, and removes the temp config
 * file generated by StartLivoxSdk().
 */
void StopLivoxSdk()
{
    if (!g_sdkInitialized) {
        AddLogMessage("SDK is not running.");
        return;
    }

    LivoxLidarSdkUninit();
    g_sdkInitialized = false;

    if (!g_tempConfigPath.empty()) {
        DeleteFileA(g_tempConfigPath.c_str());
        g_tempConfigPath.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_lidarDevicesMutex);
        g_lidarDevices.clear();
    }
    g_selectedLidarHandle = 0;
    g_rebootRequired = false;

    // Clear the IP configuration input buffers
    g_newIpBuffer[0]   = '\0';
    g_netmaskBuffer[0] = '\0';
    g_gatewayBuffer[0] = '\0';

    AddLogMessage("Disconnected from SDK.");
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
    // (widened from 800 to comfortably fit the adapter combo box plus the
    // Refresh Adapters and Connect/Disconnect buttons on a single row)
    int windowWidth = static_cast<int>(900 * dpiScale);
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
    
    // Populate the network adapter list once at startup so the dropdown is
    // not empty when the window first appears.
    RefreshNetworkAdapters();
    
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
        // Lock the main window to fill the entire OS viewport so internal
        // panels never wander or misalign when the application is resized.
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainViewport->Pos);
        ImGui::SetNextWindowSize(mainViewport->Size);
        
        ImGui::Begin("Livox IP Configurator", nullptr,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        
        // ====================================================================
        // Network Adapter Management
        // ====================================================================
        ImGui::Text("Network Adapter Management");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Build the combo preview label from the currently selected adapter
        const char* comboPreview = "<no adapters found>";
        std::string comboPreviewStorage;
        if (g_selectedAdapterIndex >= 0 && g_selectedAdapterIndex < static_cast<int>(g_networkAdapters.size())) {
            const NetworkAdapter& sel = g_networkAdapters[static_cast<size_t>(g_selectedAdapterIndex)];
            comboPreviewStorage = sel.name + " (" + sel.ipAddress + ")";
            comboPreview = comboPreviewStorage.c_str();
        }
        
        // Lock adapter selection while the SDK is running; the user must
        // disconnect first before switching to a different adapter.
        ImGui::BeginDisabled(g_sdkInitialized);
        
        ImGui::SetNextItemWidth(400.0f);
        if (ImGui::BeginCombo("Network Adapter", comboPreview)) {
            for (int i = 0; i < static_cast<int>(g_networkAdapters.size()); ++i) {
                const NetworkAdapter& adapter = g_networkAdapters[static_cast<size_t>(i)];
                std::string label = adapter.name + " (" + adapter.ipAddress + ")";
                bool isSelected = (i == g_selectedAdapterIndex);
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    g_selectedAdapterIndex = i;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Refresh Adapters")) {
            RefreshNetworkAdapters();
        }
        
        ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (!g_sdkInitialized) {
            ImGui::BeginDisabled(g_selectedAdapterIndex < 0);
            if (ImGui::Button("Connect / Start")) {
                const NetworkAdapter& adapter = g_networkAdapters[static_cast<size_t>(g_selectedAdapterIndex)];
                StartLivoxSdk(adapter.ipAddress);
            }
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Disconnect / Stop")) {
                StopLivoxSdk();
            }
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // ====================================================================
        // Discovered Sensors Table
        // ====================================================================
        ImGui::Text("Discovered Mid-360 Sensors");
        ImGui::Spacing();
        
        {
            std::lock_guard<std::mutex> lock(g_lidarDevicesMutex);

            // --- Liveness check ---
            // The SDK broadcasts a detection request every 1 second and online
            // sensors reply immediately.  If we haven't heard from a sensor in
            // 5 seconds it is almost certainly offline (rebooting, unplugged, etc.).
            static const auto kOfflineThreshold = std::chrono::seconds(5);
            auto now = std::chrono::steady_clock::now();
            for (auto& entry : g_lidarDevices) {
                entry.second.online = (now - entry.second.lastSeenTime) < kOfflineThreshold;
            }

            ImGuiTableFlags sensorTableFlags = ImGuiTableFlags_Borders
                                              | ImGuiTableFlags_RowBg
                                              | ImGuiTableFlags_Resizable;
            
            if (ImGui::BeginTable("DiscoveredSensorsTable", 3, sensorTableFlags, ImVec2(0.0f, 150.0f))) {
                ImGui::TableSetupColumn("Serial Number", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Current IP", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();
                
                for (const auto& entry : g_lidarDevices) {
                    const LidarDeviceInfo& device = entry.second;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    
                    bool isSelected = (device.handle == g_selectedLidarHandle);
                    if (ImGui::Selectable(device.serialNumber.c_str(), isSelected,
                                           ImGuiSelectableFlags_SpanAllColumns)) {
                        g_selectedLidarHandle = device.handle;
                        g_rebootRequired = false;

                        // Populate the input buffers from cached data
                        strncpy_s(g_newIpBuffer, device.currentIp.c_str(), sizeof(g_newIpBuffer) - 1);

                        if (device.ipConfigQueried) {
                            // We already have the full config cached — use it
                            strncpy_s(g_netmaskBuffer, device.currentNetmask.c_str(), sizeof(g_netmaskBuffer) - 1);
                            strncpy_s(g_gatewayBuffer, device.currentGateway.c_str(), sizeof(g_gatewayBuffer) - 1);
                        } else {
                            // Clear netmask/gateway until the query returns
                            g_netmaskBuffer[0] = '\0';
                            g_gatewayBuffer[0] = '\0';

                            // Fire an async query to get the actual netmask & gateway
                            QueryLivoxLidarInternalInfo(device.handle, OnQueryLidarInternalInfo, nullptr);
                        }
                    }
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(device.currentIp.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (device.online) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Online");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Offline");
                    }
                }
                ImGui::EndTable();
            }
            
            if (g_lidarDevices.empty()) {
                ImGui::TextDisabled("No sensors discovered yet. Connect to the SDK and wait for broadcasts.");
            }
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // ====================================================================
        // IP Configuration & Reboot Controls
        // ====================================================================
        ImGui::Text("IP Configuration");
        ImGui::Spacing();
        
        ImGui::BeginDisabled(g_selectedLidarHandle == 0);
        
        // --- Current Configuration (read-only) ---
        {
            std::lock_guard<std::mutex> lock(g_lidarDevicesMutex);
            auto it = g_lidarDevices.find(g_selectedLidarHandle);
            if (it != g_lidarDevices.end() && it->second.ipConfigQueried) {
                const LidarDeviceInfo& sel = it->second;
                ImGui::Text("Current:  IP: %s  |  Mask: %s  |  GW: %s",
                            sel.currentIp.c_str(),
                            sel.currentNetmask.c_str(),
                            sel.currentGateway.c_str());
            } else if (it != g_lidarDevices.end()) {
                ImGui::TextDisabled("Current config: querying sensor...");
            } else {
                ImGui::TextDisabled("No sensor selected.");
            }
        }
        ImGui::Spacing();
        
        // --- New Configuration (editable) ---
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("New Target IP Address", g_newIpBuffer, sizeof(g_newIpBuffer));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Subnet Mask", g_netmaskBuffer, sizeof(g_netmaskBuffer));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Gateway", g_gatewayBuffer, sizeof(g_gatewayBuffer));
        
        ImGui::Spacing();
        
        if (ImGui::Button("Push New IP")) {
            LivoxLidarIpInfo ipInfo;
            memset(&ipInfo, 0, sizeof(ipInfo));
            strncpy_s(ipInfo.ip_addr, g_newIpBuffer, sizeof(ipInfo.ip_addr) - 1);
            strncpy_s(ipInfo.net_mask, g_netmaskBuffer, sizeof(ipInfo.net_mask) - 1);
            strncpy_s(ipInfo.gw_addr, g_gatewayBuffer, sizeof(ipInfo.gw_addr) - 1);
            
            livox_status result = SetLivoxLidarIp(g_selectedLidarHandle, &ipInfo, OnSetLivoxLidarIp, nullptr);
            if (result != kLivoxLidarStatusSuccess) {
                AddLogMessage("Error: SetLivoxLidarIp() call failed to send (status:" + std::to_string(result) + ").");
            } else {
                AddLogMessage("Push New IP request sent, waiting for sensor response...");
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Reboot Sensor")) {
            livox_status result = LivoxLidarRequestReboot(g_selectedLidarHandle, OnLivoxLidarReboot, nullptr);
            if (result != kLivoxLidarStatusSuccess) {
                AddLogMessage("Error: LivoxLidarRequestReboot() call failed to send (status:" + std::to_string(result) + ").");
            } else {
                AddLogMessage("Reboot request sent, waiting for sensor response...");
            }
        }
        if (g_rebootRequired) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Reboot required for changes to take effect.");
        }
        
        ImGui::EndDisabled();
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // ====================================================================
        // Status / Log Area
        // ====================================================================
        ImGui::Text("Log");
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            ImGui::BeginChild("LogScrollArea", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders);
            for (const std::string& line : g_logMessages) {
                ImGui::TextWrapped("%s", line.c_str());
            }
            if (!g_logMessages.empty()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
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
    if (g_sdkInitialized) {
        StopLivoxSdk();
    }
    
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