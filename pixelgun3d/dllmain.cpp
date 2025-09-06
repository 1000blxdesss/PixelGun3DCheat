#include <Windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include "Logger/Logger.h"
#include "imhui/imgui.h"
#include "imhui/imgui_impl_win32.h"
#include "imhui/imgui_impl_dx11.h"
#include "imhui/imgui_impl_dx10.h"
#include "kiero/kiero.h"
#include "IL2CPPResolver/Data.hpp"
#include "IL2CPPResolver/IL2CPP_Resolver.hpp"
#include "MinHook/include/MinHook.h"
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <memory>

// Methods
#define DomainGetAssemblies 0x3af980
#define Player_move_c_Update 0x1c82200
#define Internal_SceneUnloaded 0x4480eb0
#define TextMeshGetColor 0x453ec20
#define TextMeshGetText 0x453ee30
#define ComponentGetTransform 0x4452d90
#define CollisionFlagsMove 0x44c0300
#define SetFov 0x4432380
#define get_eulerAngles 0x4467650
#define set_eulerAngles 0x4467f40
#define LookAt 0x4466190
#define get_deltaTime 0x44647a0
#define set_rotation 0x44685a0
#define get_rotation 0x4467d70
#define Raycast 0x44c5340 //Raycast(Vector3 origin, Vector3 direction, RaycastHit hitInfo, Single maxDistance, Int32 layerMask) { }
#define GetInstanceID 0x445a400
#define TransformDirection 0x4467130
// Fields
#define nickLabel 0x3E8
#define headColliderPivot 0x140
#define myPlayerTransform 0x3c8
#define headCollider 0x138
//public BoxCollider headCollider; // 0x138
//public Transform headColliderPivot; // 0x140

#define M_PI 3.14159265358979323846
int32_t layerMask = 0xFFFF;
static void* last_aim = nullptr;
static int timesa = 0;
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_showMenu = false;
uintptr_t GameBase = NULL;
uintptr_t GameAssembly = NULL;
uintptr_t UnityPlayer = NULL;
static std::vector<void*> g_hooks;
Unity::CCamera* main_camera = nullptr;
std::vector<void*> player_list;
void* myPlayer = nullptr;
float noClipSpeed = 10.0f;
std::mutex player_list_mutex;

std::mutex safe_player_data_mutex;

ID3D11Device* g_device11 = nullptr;
ID3D11DeviceContext* g_context11 = nullptr;
ID3D11RenderTargetView* g_renderTarget11 = nullptr;

ID3D10Device* g_device10 = nullptr;
ID3D10RenderTargetView* g_renderTarget10 = nullptr;

bool g_usingDx11 = false;
bool g_usingDx10 = false;

static bool aimEnabled = false;
static bool camera = false;
static bool espEnabled = false;
static bool speedEnabled = false;
static bool tracers = false;
bool noClipEnabled = false;
bool piercing = false;
float speedMul = 2.0f;
bool fovEnabled = false;

void handle_noclip();



//structs
struct PlayerData {
    void* player;
    Unity::Vector3 head_pos;
    Unity::Vector3 center_pos;
    std::string name;
    ImU32 color;
    float distance;
    bool is_teammate;
    void* collider;
};
std::vector<PlayerData> safe_player_data;
// vro wat
static struct ModuleConfig {
    struct ESP {
        bool enabled = false;
        bool showName = true;
        bool showHeadPoint = true;
        bool showTeammates = false;
        int boxStyle = 0; // 0: Full Box, 1: Corner Box, 2: Filled Box
        bool showDistance = true;
        bool showTracers = false;
        float transparency = 0.8f;
        ImColor enemyColor = ImColor(255, 0, 0, 255);
        ImColor teammateColor = ImColor(0, 255, 0, 255);
    } esp;

    struct Aimbot {
        float radius = 100.0f;
        float smoth = 1.0f;
        bool wall = true;
        ImColor radiusColor = ImColor(255, 255, 0, 128);

    }aimbot;

    struct Camerrra
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float fovSc = 10.0f;
    }camera;

} config;


struct AimTarget {
    Unity::Vector3 head_pos;
    Unity::Vector3 head_screen;
    float screen_distance;
};
static AimTarget aim_target = {};

struct RaycastHit {
    Unity::Vector3 m_Point;     // 0x10
    Unity::Vector3 m_Normal;    // 0x1C
    uint32_t m_FaceID;          // 0x28
    float m_Distance;           // 0x2C
    Unity::Vector2 m_UV;        // 0x30
    int32_t m_Collider;         // 0x38
};


namespace UnityUtils
{
    inline Unity::Color TextMeshGetColor_(void* text_mesh_instance) {
        static const auto fn = (Unity::Color(*)(void*)) (GameAssembly + TextMeshGetColor);
        if (!fn || !text_mesh_instance) return { 1.0f, 1.0f, 1.0f, 1.0f };
        return fn(text_mesh_instance);
    }

    void* TextMeshGetText_(void* arg) {
        if (!arg) return nullptr;
        static const auto fn = (void* (*)(void*)) (GameAssembly + TextMeshGetText);
        return fn(arg);
    }

    inline Unity::CTransform* ComponentGetTransform_(void* component_instance) {
        static const auto fn = (Unity::CTransform * (*)(void*)) (GameAssembly + ComponentGetTransform);
        if (!fn || !component_instance) return nullptr;
        return fn(component_instance);
    }

    void* get_transform(void* player) { // -> GetTransform???
        return (void*)*(uint64_t*)((uint64_t)player + myPlayerTransform);
    }
    static Unity::Vector3 TransformGetPosition_(void* transform) {
        static const auto fn = (Unity::Vector3(*)(void*))(GameAssembly + 0x4467c00); // RVA: 0x4467c00
        if (!fn || !transform || IsBadReadPtr(transform, sizeof(Unity::CTransform))) {
            return { 0.0f, 0.0f, 0.0f };
        }
        return fn(transform);
    }
}
namespace Functions
{
    static Unity::Vector3 TransformGetEulerAngles(void* transform) {
        static const auto fn = (Unity::Vector3(*)(void*))(GameAssembly + get_eulerAngles);
        if (!fn) return { 0, 0, 0 };
        return fn(transform);
    }

    static void TransformSetEulerAngles(void* transform, Unity::Vector3* euler) {
        static const auto fn = (void (*)(void*, Unity::Vector3*))(GameAssembly + set_eulerAngles);
        if (fn) fn(transform, euler);
    }

    static void TransformLookAt(void* transform, Unity::Vector3* target, Unity::Vector3* up) {
        static const auto fn = (void (*)(void*, Unity::Vector3*, Unity::Vector3*))(GameAssembly + LookAt);
        if (fn) fn(transform, target, up);
    }

    static float get_deltaTime_() {
        static const auto fn = (float(*)())(GameAssembly + get_deltaTime);
        if (fn) return fn();
    }
    static void TransformSetRotation(void* arg, void* quaternion)
    {
        if (!arg) return;
        static const auto fn = (void(*)(void*, void*)) (GameAssembly + set_rotation);
        return fn(arg, quaternion);
    }
    static void TransformGetRotation(void* arg, void* quaternion)
    {
        if (!arg) return;
        static const auto fn = (void(*)(void*, void*)) (GameAssembly + get_rotation);
        return fn(arg, quaternion);
    }
    static bool PhysicsRaycast(Unity::Vector3 origin, Unity::Vector3 direction, RaycastHit* hitInfo, float maxDistance, int32_t layerMask) {
        static const auto fn = (bool (*)(Unity::Vector3, Unity::Vector3, RaycastHit*, float, int32_t))(GameAssembly + Raycast);
        if (!fn) {
            return false;
        }
        return fn(origin, direction, hitInfo, maxDistance, layerMask);
    }
    static int ObjectGetInstanceID(void* arg)
    {
        if (!arg) return -1;
        static const auto fn = (int(*)(void*))(GameAssembly + GetInstanceID);
        return fn(arg);
    }
    static Unity::Vector3 TransformDirection_(void* transform, Unity::Vector3 direction) {
        static const auto fn = (Unity::Vector3(*)(void*, Unity::Vector3))(GameAssembly + TransformDirection);
        if (!fn)  return { 0, 0, 0 };
        return fn(transform, direction);
    }
}

namespace something_useful
{
    static bool esp_on_screen(const Unity::Vector3& pos) {
        return pos.x >= -50.0f && pos.x <= ImGui::GetIO().DisplaySize.x + 50.0f && pos.y >= -50.0f && pos.y <= ImGui::GetIO().DisplaySize.y + 50.0f && pos.z > 0.01f;
    }

    static float vec3_distance(const Unity::Vector3& one, const Unity::Vector3& two) {
        return (float)abs(sqrt(pow(one.x - two.x, 2) + pow(one.y - two.y, 2) + pow(one.z - two.z, 2)));
    }

    std::string CleanString(std::string string) {
        if (string.size() > 524288) {
            return "";
        }
        std::vector<char> bytes(string.begin(), string.end());
        bytes.push_back('\0');
        std::vector<char> chars;
        for (char byte : bytes) {
            if (byte) {
                chars.push_back(byte);
            }
        }
        return std::string(chars.begin(), chars.end());
    }
    static float to_rad(float angle) {
        return angle * (float)(M_PI / 180);
    }

    static float to_deg(float angle) {
        return angle * (float)(180 / M_PI);
    }

    static float lerp_angle(float theta1, float theta2, float ratio) {
        float max = M_PI * 2;
        float da = fmod(theta2 - theta1, max);
        float dist = fmod(2 * da, max) - da;
        return theta1 + dist * ratio;
    }

    static float vec3_distance(Unity::Vector3& one, Unity::Vector3& two) {
        return (float)abs(sqrt(pow(one.x - two.x, 2) + pow(one.y - two.y, 2) + pow(one.z - two.z, 2)));
    }

    float clamp(float value, float min, float max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
}

LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// чем ужаснее игра, тем страшнее методы
LRESULT WINAPI HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_INSERT) {
            g_showMenu = !g_showMenu;
            ImGuiIO& io = ImGui::GetIO();
            io.MouseDrawCursor = g_showMenu;
            if (g_showMenu) {
                while (ShowCursor(TRUE) < 0);
                ClipCursor(nullptr);
            }
            else {
                while (ShowCursor(FALSE) > -2);
                RECT rect;
                GetClientRect(hWnd, &rect);
                POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
                ClientToScreen(hWnd, &center);
                SetCursorPos(center.x, center.y);
                POINT topLeft = { rect.left, rect.top };
                POINT bottomRight = { rect.right, rect.bottom };
                ClientToScreen(hWnd, &topLeft);
                ClientToScreen(hWnd, &bottomRight);
                RECT gameRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
                ClipCursor(&gameRect);
            }
            return 0;
        }
        else if (wParam == VK_END) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        if (g_showMenu) {
            ImGuiIO& io = ImGui::GetIO();
            POINT mousePos;
            GetCursorPos(&mousePos);
            ScreenToClient(hWnd, &mousePos);
            io.MousePos = ImVec2(static_cast<float>(mousePos.x), static_cast<float>(mousePos.y));
            io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            return 1;
        }
        break;
    case WM_SETCURSOR:
        if (g_showMenu)
            return 1;
        break;
    }
    return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
}

//std::string get_player_name(void* player_move_c) {
//    return player_move_c && (*(uint64_t*)((uint64_t)player_move_c + nickLabel)) && UnityUtils::TextMeshGetText_((void*)*(uint64_t*)((uint64_t)player_move_c + nickLabel)) ? something_useful::CleanString(((Unity::System_String*)UnityUtils::TextMeshGetText_((void*)*(uint64_t*)((uint64_t)player_move_c + nickLabel)))->ToString()) : "";
//}

std::string get_player_name(void* player_move_c) {
    if (player_move_c == nullptr) return "";
    void* nick_label = (void*)*(uint64_t*)((uint64_t)player_move_c + nickLabel);
    void* name_ptr = UnityUtils::TextMeshGetText_(nick_label);
    if (name_ptr == nullptr) return "";
    std::string name = ((Unity::System_String*)name_ptr)->ToString();
    return something_useful::CleanString(name);
}

bool is_my_player_move_c(void* player_move_c) {
    return get_player_name(player_move_c) == "#Player Nickname";
}
void SetCustomStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 12.0f;
    style.WindowRounding = 15.0f;
    style.GrabRounding = 10.0f;
    style.ChildRounding = 12.0f;
    style.ItemSpacing = ImVec2(10, 10);
    style.FramePadding = ImVec2(12, 6);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.70f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.8f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

void initIl2цп() {
    GameBase = (uintptr_t)GetModuleHandleA(NULL);
    GameAssembly = (uintptr_t)GetModuleHandleA("GameAssembly.dll");
    UnityPlayer = (uintptr_t)GetModuleHandleA("UnityPlayer.dll");
    IL2CPP::Functions.m_DomainGetAssemblies = (void*)((uintptr_t)GameAssembly + DomainGetAssemblies);
    if (IL2CPP::Initialize()) {
        Logger::log_info("IL2CPP initialized successfully!");
    }
    else {
        Logger::log_err("IL2CPP initialization failed!");
    }
}
// hook se(x)ction
inline void(__stdcall* CollisionFlags_Move_orig)(void* characterController, Unity::Vector3* value);
inline void __stdcall CollisionFlags_Move(void* characterController, Unity::Vector3* value) {
    if (noClipEnabled) {
        return;
    }
    if (speedEnabled) {
        value->x *= speedMul;
        value->y *= speedMul;
        value->z *= speedMul;
    }
    return CollisionFlags_Move_orig(characterController, value);
}
inline void(__stdcall* player_move_c_original)(void* arg);
inline void __stdcall player_move_c(void* arg) {
    main_camera = Unity::Camera::GetMain();
    if (!main_camera) {
        std::lock_guard<std::mutex> lock(player_list_mutex);
        player_list.clear();
        std::lock_guard<std::mutex> lock2(safe_player_data_mutex);
        safe_player_data.clear();
        return player_move_c_original(arg);
    }

    bool my_player = is_my_player_move_c(arg);
    if (my_player) {
        myPlayer = arg;
        handle_noclip();
    }


    std::lock_guard<std::mutex> lock(player_list_mutex);
    auto it = std::find(player_list.begin(), player_list.end(), arg);
    if (it == player_list.end()) {
        player_list.push_back(arg);
    }

    return player_move_c_original(arg);
}

inline __int64(__fastcall* Internal_SceneUnloaded_original)(void* arg);
inline __int64 __fastcall Internal_SceneUnloaded_(void* arg) {
    std::lock_guard<std::mutex> lock(player_list_mutex);
    player_list.clear();
    std::lock_guard<std::mutex> lock2(safe_player_data_mutex);
    safe_player_data.clear();
    main_camera = nullptr;
    aimEnabled = false;
    espEnabled = false;
    return Internal_SceneUnloaded_original(arg);
}

/////////////////

void hook_function(uint64_t offset, LPVOID detour, void* original) {
    MH_STATUS create_hook = MH_CreateHook((LPVOID*)(GameAssembly + offset), detour, (LPVOID*)original);
    if (create_hook == MH_OK) {
        MH_EnableHook((LPVOID*)(GameAssembly + offset));
        g_hooks.push_back((LPVOID*)(GameAssembly + offset));
    }
    else {
        std::stringstream hexified;
        hexified << std::hex << offset;
        Logger::log_err("MinHook failed to hook to offset 0x" + hexified.str() + "! (Status: " + std::to_string(create_hook) + ")");
    }
}



void UpdatePlayerData() 
{
   
}
void DrawESP(const ModuleConfig& config) {
    if (!main_camera || !Unity::Camera::GetMain()) return;
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    std::lock_guard<std::mutex> lock(safe_player_data_mutex);
    safe_player_data.clear(); 
    aim_target = {};

    std::lock_guard<std::mutex> pl_lock(player_list_mutex);
    Unity::CTransform* camera_transform = UnityUtils::ComponentGetTransform_(main_camera);
    if (!camera_transform) return;
    Unity::Vector3 camera_pos = camera_transform->GetPosition();

    ImVec2 screen_center = ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2);
    float closest_dist = config.aimbot.radius + 1.0f;

    for (const auto& player : player_list) {
        if (!player || player == myPlayer) continue;
        PlayerData data;
        Unity::CTransform* head_transform = *(Unity::CTransform**)((uintptr_t)player + headColliderPivot);
        Unity::CTransform* center_transform = static_cast<Unity::CTransform*>(UnityUtils::get_transform(player));
        if (!head_transform || !center_transform) continue;

        data.head_pos = UnityUtils::TransformGetPosition_(head_transform);
        data.center_pos = UnityUtils::TransformGetPosition_(center_transform);
        data.name = get_player_name(player);
        data.distance = something_useful::vec3_distance(data.center_pos, camera_pos);
        void* nick_label = (void*)*(uint64_t*)((uint64_t)player + nickLabel);
        Unity::Color text_color = UnityUtils::TextMeshGetColor_(nick_label);
        data.color = ImGui::ColorConvertFloat4ToU32({ text_color.r, text_color.g, text_color.b, text_color.a });
        data.is_teammate = (text_color.r < 0.1f && text_color.g < 0.1f && text_color.b > 0.9f);
        data.player = player;
        if (!config.esp.showTeammates && data.is_teammate) continue;

        safe_player_data.push_back(data);
    }

    for (const auto& data : safe_player_data) {
        try {
            Unity::Vector3 head_screen, center_screen;
            Unity::Vector3 zcx = data.head_pos;
            Unity::Vector3 xzcx = data.center_pos;
            main_camera->WorldToScreen(zcx, head_screen, Unity::m_eCameraEye_Center);
            main_camera->WorldToScreen(xzcx, center_screen, Unity::m_eCameraEye_Center);
            if (!something_useful::esp_on_screen(head_screen) || !something_useful::esp_on_screen(center_screen)) continue;

          

            head_screen.y = ImGui::GetIO().DisplaySize.y - head_screen.y;
            center_screen.y = ImGui::GetIO().DisplaySize.y - center_screen.y;

            float head_to_center_distance = std::abs(head_screen.y - center_screen.y);
            float extra_distance = head_to_center_distance * 0.5f;
            float height = (head_to_center_distance * 2.0f) + (extra_distance * 2.0f);
            float width = height * 0.6f;

     
            ImVec2 box_pos = ImVec2(center_screen.x - width / 2, center_screen.y + head_to_center_distance + extra_distance - height);
            ImVec2 box_size = ImVec2(width, height);
  
            ImU32 color = data.is_teammate ? ImGui::ColorConvertFloat4ToU32(ImVec4(config.esp.teammateColor)) : ImGui::ColorConvertFloat4ToU32(ImVec4(config.esp.enemyColor));
            ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
            color_vec.w = config.esp.transparency;
            ImColor final_color = ImColor(color_vec);

            switch (config.esp.boxStyle) {
            case 0:
                draw_list->AddRect(box_pos, ImVec2(box_pos.x + box_size.x, box_pos.y + box_size.y), final_color, 0.0f, ImDrawFlags_None, 1.5f);
                break;
            case 1: 
            {
                float corner_length = box_size.y * 0.2f;
                float corner_width = box_size.x * 0.2f;
                draw_list->AddLine(box_pos, ImVec2(box_pos.x + corner_width, box_pos.y), final_color, 1.5f);
                draw_list->AddLine(box_pos, ImVec2(box_pos.x, box_pos.y + corner_length), final_color, 1.5f);
                draw_list->AddLine(ImVec2(box_pos.x + box_size.x - corner_width, box_pos.y), ImVec2(box_pos.x + box_size.x, box_pos.y), final_color, 1.5f);
                draw_list->AddLine(ImVec2(box_pos.x + box_size.x, box_pos.y), ImVec2(box_pos.x + box_size.x, box_pos.y + corner_length), final_color, 1.5f);
                draw_list->AddLine(ImVec2(box_pos.x, box_pos.y + box_size.y - corner_length), ImVec2(box_pos.x, box_pos.y + box_size.y), final_color, 1.5f);
                draw_list->AddLine(ImVec2(box_pos.x, box_pos.y + box_size.y), ImVec2(box_pos.x + corner_width, box_pos.y + box_size.y), final_color, 1.5f);
                draw_list->AddLine(ImVec2(box_pos.x + box_size.x - corner_width, box_pos.y + box_size.y), ImVec2(box_pos.x + box_size.x, box_pos.y + box_size.y), final_color, 1.5f);
                draw_list->AddLine(ImVec2(box_pos.x + box_size.x, box_pos.y + box_size.y - corner_length), ImVec2(box_pos.x + box_size.x, box_pos.y + box_size.y), final_color, 1.5f);
                break;
            }
            case 2:
                draw_list->AddRectFilled(box_pos, ImVec2(box_pos.x + box_size.x, box_pos.y + box_size.y), final_color);
                break;
            }


            if (config.esp.showName || config.esp.showDistance) {
                std::string text_to_draw = data.name;
                if (config.esp.showDistance && !data.name.empty()) {
                    text_to_draw += " [" + std::to_string(static_cast<int>(data.distance)) + "m]";
                }
                else if (config.esp.showDistance) {
                    text_to_draw = "[" + std::to_string(static_cast<int>(data.distance)) + "m]";
                }
                if (!text_to_draw.empty()) {
                    ImVec2 text_size = ImGui::CalcTextSize(text_to_draw.c_str());
                    ImVec2 text_pos = ImVec2(center_screen.x - text_size.x / 2, box_pos.y + box_size.y);
                    draw_list->AddText(text_pos, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)), text_to_draw.c_str());
                }
            }

            if (config.esp.showHeadPoint) {
                draw_list->AddCircleFilled(ImVec2(head_screen.x, head_screen.y), 3.0f, final_color);
            }

            if (tracers) {
                ImVec2 screen_center = ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y);
                draw_list->AddLine(screen_center, ImVec2(center_screen.x, center_screen.y), final_color, 1.0f);
            }
        }
        catch (...) {
;
        }
    }
}




// RVA: 0x4432380 VA: 0x7ffa9e1c2380
//public Void set_fieldOfView(Single value) {}
//// RVA: 0x4467130 VA: 0x7ffa9e1f7130
//public Vector3 TransformDirection(Vector3 direction) {}

//private class Object
//{
//    // Fields
//    private IntPtr m_CachedPtr; // 0x10
//    internal static Int32 OffsetOfInstanceIDInCPlusPlusObject; // 0x0
//    private const String objectIsNullMessage = "The Object you want to instantiate is null."; // 0x0
//    private const String cloneDestroyedMessage = "Instantiate failed because the clone was destroyed during creation. This can happen if DestroyImmediate is called in MonoBehaviour.Awake."; // 0x0
//
//    // Properties (Currently Unavailable)
//
//    // Methods
//    // RVA: 0x445a400 VA: 0x7ffa9e1ea400
//    public Int32 GetInstanceID() {}


//// RVA: 0x4467d70 VA: 0x7ffa9e1f7d70
//public Quaternion get_rotation() {}



void Aim() {
    if (!aimEnabled || !main_camera || !Unity::Camera::GetMain()) {
        return;
    }

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    ImVec2 screen_center = ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2);

    if (config.aimbot.radius > 0.0f) {
        ImU32 radius_color = ImGui::ColorConvertFloat4ToU32(config.aimbot.radiusColor);
        draw_list->AddCircle(screen_center, config.aimbot.radius, radius_color, 64, 2.0f);
    }

    PlayerData* closest_player_data = nullptr;
    float closest_dist_sq = config.aimbot.radius * config.aimbot.radius;
    std::lock_guard<std::mutex> lock(safe_player_data_mutex);

    Unity::CTransform* camera_transform = UnityUtils::ComponentGetTransform_(main_camera);
    if (!camera_transform) {
 
        return;
    }
    Unity::Vector3 camera_pos = camera_transform->GetPosition();
    for (auto& data : safe_player_data) {
        if (data.is_teammate) continue;

        if (data.head_pos.y < -9000.0f) {
            continue;
        }

        Unity::Vector3 head_screen;
        main_camera->WorldToScreen(data.head_pos, head_screen, Unity::m_eCameraEye_Center);
        head_screen.y = ImGui::GetIO().DisplaySize.y - head_screen.y;

        Unity::Vector3 direction = Unity::Vector3Subtract(data.head_pos, camera_pos);
        direction = Unity::Vector3Normalize(direction);
        float distance = something_useful::vec3_distance(camera_pos, data.head_pos);
        /*if (!config.aimbot.wall) {
            Unity::Vector3 diff = Unity::Vector3Subtract(data.head_pos, camera_pos);
            Unity::Vector3 direction = Unity::Vector3Normalize(diff);
            float distance = something_useful::vec3_distance(camera_pos, data.head_pos);
            RaycastHit hit;
            bool hitSomething = Functions::PhysicsRaycast(camera_pos, direction, &hit, distance, layerMask);
            if (hitSomething && hit.m_Collider != 0) {
                int32_t hit_id = hit.m_Collider;
                void* head_collider = nullptr;
                if (data.player) {
                    head_collider = *(void**)((uintptr_t)data.player + headCollider);
                }
                int32_t player_id = head_collider ? Functions::ObjectGetInstanceID(head_collider) : -1;
                if (player_id == -1) {
                    continue;
                }
                if (hit_id != player_id) {
                    
                    continue;
                }
               
            }
            
        }*/

        if (!something_useful::esp_on_screen(head_screen)) continue;

        float dist_to_center_sq = pow(head_screen.x - screen_center.x, 2) + pow(head_screen.y - screen_center.y, 2);

        if (dist_to_center_sq < closest_dist_sq) {
            closest_dist_sq = dist_to_center_sq;
            closest_player_data = &data;
        }
    }

    if (closest_player_data) {
        Unity::Vector3 aim_at = closest_player_data->head_pos;
        Unity::CTransform* camera_transform = UnityUtils::ComponentGetTransform_(main_camera);
        if (camera_transform != nullptr) {
            Unity::Quaternion camera_rotation = camera_transform->GetRotation();
            Unity::Vector3 up = { 0, 1, 0 };
            Functions::TransformLookAt(camera_transform, &aim_at, &up);

            Unity::Quaternion camera_rotation2 = camera_transform->GetRotation();;

            if (closest_player_data != last_aim && closest_player_data != nullptr) {
                timesa = 0;
            }
            last_aim = closest_player_data;
            timesa++;


            int smoothing = config.aimbot.smoth * 2;
            if (timesa > smoothing) timesa = smoothing;

            Unity::Vector3 euler1 = camera_rotation.ToEuler();
            Unity::Vector3 euler2 = camera_rotation2.ToEuler();
            Unity::Vector3 real_rot_euler = {
                something_useful::to_deg(something_useful::lerp_angle(something_useful::to_rad(euler1.x), something_useful::to_rad(euler2.x), (float)timesa / smoothing)),
                something_useful::to_deg(something_useful::lerp_angle(something_useful::to_rad(euler1.y), something_useful::to_rad(euler2.y), (float)timesa / smoothing)),
                something_useful::to_deg(something_useful::lerp_angle(something_useful::to_rad(euler1.z), something_useful::to_rad(euler2.z), (float)timesa / smoothing)),
            };

            Unity::Quaternion real_rot = { 0, 0, 0, 0 }; 
            real_rot = real_rot.Euler(real_rot_euler);
            camera_transform->SetRotation(real_rot);
           // Functions::TransformSetRotation(camera_transform, &real_rot);
            
            
        }
        else {
            last_aim = nullptr;
            timesa = 0;
        }
    }
}

void update_camera_position() {
    if (!myPlayer || !main_camera) return;


    Unity::CTransform* player_transform = static_cast<Unity::CTransform*>(UnityUtils::get_transform(myPlayer));
    Unity::CTransform* camera_transform = UnityUtils::ComponentGetTransform_(main_camera);

    if (!player_transform || !camera_transform) return;

   
    Unity::Vector3 local_offset = { config.camera.x, config.camera.y, config.camera.z };

    Unity::Vector3 world_offset = Functions::TransformDirection_(player_transform, local_offset);


    Unity::Vector3 new_camera_position = Unity::Vector3Add(player_transform->GetPosition(), world_offset);

    camera_transform->SetPosition(new_camera_position);
    Unity::Vector3 target_pos = *(Unity::Vector3*)((uintptr_t)myPlayer + headColliderPivot);
    Unity::Vector3 up_vector = { 0, 1, 0 };
    Functions::TransformLookAt(camera_transform, &target_pos, &up_vector);
}


void handle_noclip() {
    if (!noClipEnabled) return;

    Unity::CTransform* player_transform = static_cast<Unity::CTransform*>(UnityUtils::get_transform(myPlayer));
    Unity::CTransform* camera_transform = UnityUtils::ComponentGetTransform_(main_camera);
    if (!player_transform || !camera_transform) return;

    Unity::Vector3 movement = { 0.0f, 0.0f, 0.0f };
    float speed = noClipSpeed * Functions::get_deltaTime_();

    if (GetAsyncKeyState('W')) {
        movement.z += speed;
    }
    if (GetAsyncKeyState('S')) {
        movement.z -= speed;
    }
    if (GetAsyncKeyState('A')) {
        movement.x -= speed;
    }
    if (GetAsyncKeyState('D')) {
        movement.x += speed;
    }


    if (GetAsyncKeyState(VK_SPACE)) {
        movement.y += speed;
    }
    if (GetAsyncKeyState(VK_CONTROL)) {
        movement.y -= speed;
    }

    if (movement.x != 0.0f || movement.y != 0.0f || movement.z != 0.0f) {
        Unity::Vector3 world_movement = Functions::TransformDirection_(camera_transform, movement);
        Unity::Vector3 new_pos = Unity::Vector3Add(player_transform->GetPosition(), world_movement);
        player_transform->SetPosition(new_pos);
    }
}



void init_hooks() {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Logger::log_err("MinHook initialization failed! Status: " + std::to_string(status));
        return;
    }
    Logger::log_info("MinHook initialized successfully!");
    hook_function(Player_move_c_Update, &player_move_c, &player_move_c_original);
    hook_function(Internal_SceneUnloaded, &Internal_SceneUnloaded_, &Internal_SceneUnloaded_original);
    hook_function(CollisionFlagsMove, &CollisionFlags_Move, &CollisionFlags_Move_orig);
    Logger::log_info("All hooks created");
}

void InitImGuiDX11() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_showMenu;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device11, g_context11);
    g_usingDx11 = true;
    Logger::log_info("ImGui DX11 initialized");
}

void InitImGuiDX10() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_showMenu;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX10_Init(g_device10);
    g_usingDx10 = true;
    Logger::log_info("ImGui DX10 initialized");
}

void RenderGUI() {
    if (g_usingDx11)
        ImGui_ImplDX11_NewFrame();
    else if (g_usingDx10)
        ImGui_ImplDX10_NewFrame();

    SetCustomStyle();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_showMenu;
    io.WantCaptureMouse = g_showMenu;

    if (g_showMenu) {
        ImGui::Begin("Simple Menu", &g_showMenu, ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::BeginTabBar("##MainTabBar")) {
            if (ImGui::BeginTabItem("Combat")) {
                ImGui::Checkbox("Aim", &aimEnabled);
               
                if (aimEnabled) {
                    ImGui::Separator();
                    ImGui::Text("Aim settings:");
                    ImGui::SliderFloat("Radius", &config.aimbot.radius, 10.0f, 500.0f, "%.0f px");
                    ImGui::SliderFloat("Smoothness", &config.aimbot.smoth, 1.0f, 2000.0f);
                    ImGui::Checkbox("Through walls", &config.aimbot.wall);
                    ImGui::ColorEdit4("Radius color", (float*)&config.aimbot.radiusColor);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Movement")) {
                ImGui::Checkbox("Speed", &speedEnabled);
                ImGui::Checkbox("NoClip", &noClipEnabled);
                if (speedEnabled) 
                {
                    ImGui::SliderFloat("x", &speedMul, 0.0f, 100.0f);
                }
                if (noClipEnabled) 
                {
                    ImGui::SliderFloat("x", &noClipSpeed, 0.0f, 100.0f);
                    handle_noclip();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Visual")) {
                ImGui::Checkbox("ESP", &espEnabled);
                ImGui::Checkbox("Camera", &camera);
                
                if (camera) 
                {
                    ImGui::SliderFloat("X", &config.camera.x, -100.0f, 100.0f);
                    ImGui::SliderFloat("Y", &config.camera.y, -100.0f, 100.0f);
                    ImGui::SliderFloat("Z", &config.camera.z, -100.0f, 100.0f);
                    update_camera_position();
                }
               
                if (espEnabled) {
                    ImGui::Separator();
                    ImGui::Text("ESP Settings:");
                    ImGui::SliderFloat("Transparency", &config.esp.transparency, 0.1f, 1.0f, "%.1f");
                    const char* boxStyles[] = { "Full Box", "Corner Box", "Filled Box" };
                    ImGui::Combo("Box Style", &config.esp.boxStyle, boxStyles, IM_ARRAYSIZE(boxStyles));
                    ImGui::Checkbox("Show Names", &config.esp.showName);
                    ImGui::Checkbox("Show Head Point", &config.esp.showHeadPoint);
                    ImGui::Checkbox("Show Distance", &config.esp.showDistance);
                    ImGui::Checkbox("Show Teammates", &config.esp.showTeammates);
                    ImGui::Separator();
                    ImGui::Text("Colors:");
                    ImGui::ColorEdit4("Enemy Color", (float*)&config.esp.enemyColor);
                    ImGui::ColorEdit4("Teammate Color", (float*)&config.esp.teammateColor);
                }
                ImGui::Checkbox("Tracers", &tracers);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    if (espEnabled) {
        DrawESP(config);
    }
    if (aimEnabled) {
        Aim();
    }

    ImGui::Render();
    if (g_usingDx11) {
        g_context11->OMSetRenderTargets(1, &g_renderTarget11, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    else if (g_usingDx10) {
        g_device10->OMSetRenderTargets(1, &g_renderTarget10, nullptr);
        ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
    }
}

HRESULT(__stdcall* oPresent11)(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
HRESULT __stdcall hkPresent11(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static bool initialized = false;
    if (!initialized) {
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device11))) {
            g_device11->GetImmediateContext(&g_context11);
            DXGI_SWAP_CHAIN_DESC desc;
            swapChain->GetDesc(&desc);
            g_window = desc.OutputWindow;
            g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
            ID3D11Texture2D* backBuffer;
            swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
            g_device11->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget11);
            backBuffer->Release();
            InitImGuiDX11();
            initialized = true;
        }
    }
    RenderGUI();
    return oPresent11(swapChain, syncInterval, flags);
}
HRESULT(__stdcall* oPresent10)(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

HRESULT __stdcall hkPresent10(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static bool initialized = false;
    if (!initialized) {
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&g_device10))) {
            DXGI_SWAP_CHAIN_DESC desc;
            swapChain->GetDesc(&desc);
            g_window = desc.OutputWindow;
            g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
            ID3D10Texture2D* backBuffer;
            swapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&backBuffer);
            g_device10->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget10);
            backBuffer->Release();
            InitImGuiDX10();
            initialized = true;
        }
    }
    RenderGUI();
    return oPresent10(swapChain, syncInterval, flags);
}

void Cleanup() {
    Logger::log_info("Cleaning up...");
    g_showMenu = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (g_usingDx11) {
        kiero::unbind(8);
        ImGui_ImplDX11_Shutdown();
    }
    else if (g_usingDx10) {
        kiero::unbind(8);
        ImGui_ImplDX10_Shutdown();
    }

    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_renderTarget11) {
        g_renderTarget11->Release();
        g_renderTarget11 = nullptr;
    }
    if (g_renderTarget10) {
        g_renderTarget10->Release();
        g_renderTarget10 = nullptr;
    }
    g_device11 = nullptr;
    g_context11 = nullptr;
    g_device10 = nullptr;

    if (g_window && g_originalWndProc) {
        SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
        Logger::log_info("WndProc restored");
    }

    for (void* hookAddress : g_hooks) {
        MH_DisableHook(hookAddress);
        MH_RemoveHook(hookAddress);
    }
    g_hooks.clear();
    MH_Uninitialize();
    Logger::log_info("MinHook removed");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Logger::log_info("DLL Unloaded");
}

DWORD WINAPI UnloadThread(LPVOID param) {
    Sleep(500);
    FreeLibraryAndExitThread((HMODULE)param, 0);
    return 0;
}

DWORD WINAPI MainThread(LPVOID param) {
    AllocConsole();
    FILE* console;
    freopen_s(&console, "CONOUT$", "w", stdout);
    Logger::console = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(Logger::console, 0x000F);
    Logger::log_info("DLL Injected successfully!");

    initIl2цп();
    init_hooks();

    if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success) {
        kiero::bind(8, (void**)&oPresent11, hkPresent11);
        Logger::log_info("Using DX11");
    }
    else if (kiero::init(kiero::RenderType::D3D10) == kiero::Status::Success) {
        kiero::bind(8, (void**)&oPresent10, hkPresent10);
        Logger::log_info("Using DX10");
    }
    else {
        Logger::log_err("No DirectX support found!");
        if (console) fclose(console);
        FreeConsole();
        return 0;
    }

    while (!GetAsyncKeyState(VK_END)) {
        Sleep(50);
    }

    Cleanup();
    if (console) fclose(console);
    FreeConsole();
    CreateThread(nullptr, 0, UnloadThread, param, 0, nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, MainThread, module, 0, nullptr);
    }
    return TRUE;
}