#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_node_editor.h"
#include "graph.h"
#include "toolbox.h"
#include "nodes.h"
#include "nodelogic.h"
#include "customnodes.h"
#include "serialize.h"
#include "app.h"
#include <d3d11.h>
#include <tchar.h>
#include <atomic>
#include <filesystem>
#include <list>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ax::NodeEditor;

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct ScriptTab {
    std::string         Name = "Unsaved Script";
    std::string         Path;              // empty = unsaved
    Editor::Graph       Graph;
    ed::EditorContext*  Ctx = nullptr;
    bool                Open = true;

    ScriptTab() = default;
    ScriptTab(const ScriptTab&) = delete;
    ScriptTab& operator=(const ScriptTab&) = delete;
    ScriptTab(ScriptTab&& o) noexcept
        : Name(std::move(o.Name)), Path(std::move(o.Path)), Graph(std::move(o.Graph)),
          Ctx(o.Ctx), Open(o.Open) {
        o.Ctx = nullptr;
    }
};

static std::list<ScriptTab> g_Tabs;
static auto g_Active = g_Tabs.begin();

static bool g_ShowLoad = false;
static bool g_ShowSaveAs = false;
static char g_SaveAsName[128] = "";
static ScriptTab* g_CtxTab = nullptr;   // tab targeted by the context menu

static ScriptTab& NewTab(const char* name = nullptr) {
    ScriptTab t;
    if (name) t.Name = name;
    t.Ctx = ed::CreateEditor();
    g_Tabs.push_back(std::move(t));
    ScriptTab& tab = g_Tabs.back();
    Nodes::SpawnScriptStartNode(tab.Graph);
    g_Active = std::prev(g_Tabs.end());
    return tab;
}

static void StopTabHooks(ScriptTab& tab) {
    for (Editor::Node& n : tab.Graph.Nodes)
        Exec::StopHook(n);
}

static void CloseTab(std::list<ScriptTab>::iterator it) {
    StopTabHooks(*it);
    ed::DestroyEditor(it->Ctx);
    bool wasActive = (it == g_Active);
    it = g_Tabs.erase(it);
    if (g_Tabs.empty()) {
        NewTab();
        g_Active = std::prev(g_Tabs.end());
    } else if (wasActive) {
        g_Active = (it == g_Tabs.end()) ? std::prev(g_Tabs.end()) : it;
    }
}

static void ApplyNodePositions(ScriptTab& tab) {
    ed::SetCurrentEditor(tab.Ctx);
    for (Editor::Node& n : tab.Graph.Nodes)
        ed::SetNodePosition(n.ID, n.Pos);
}

static void SaveActiveAs(const std::string& name) {
    std::string path = (fs::path(IO::ScriptsDir()) / (name + ".json")).string();
    if (IO::SaveGraph(g_Active->Graph, path)) {
        g_Active->Path = path;
        g_Active->Name = name;
        g_Active->Graph.Dirty = false;
    }
    g_ShowSaveAs = false;
}

// remove every instance of a deleted custom node def from all open scripts
static void PurgeCustomInstances(const std::string& defName) {
    for (ScriptTab& tab : g_Tabs) {
        auto& nodes = tab.Graph.Nodes;
        for (auto it = nodes.begin(); it != nodes.end();) {
            if (it->Type == defName) {
                Editor::RemoveLinksTouchingNode(tab.Graph, *it);
                it = nodes.erase(it);
                tab.Graph.Dirty = true;
            } else {
                ++it;
            }
        }
    }
}

static const char* kVarTypeNames[] = { "Any", "Bool", "Int", "Float", "String", "Name", "Object" };
static const Editor::PinType kVarTypes[] = { Editor::PinType::Any, Editor::PinType::Bool, Editor::PinType::Int,
    Editor::PinType::Float, Editor::PinType::String, Editor::PinType::Name, Editor::PinType::Object };

static void RenderVariablesPanel(ScriptTab& tab) {
    ImGui::Begin("Variables");
    if (ImGui::Button("+ Add Variable")) {
        std::lock_guard<std::mutex> lock(tab.Graph.VarsMutex);
        std::string base = "NewVar";
        std::string name = base;
        int i = 1;
        while (tab.Graph.Variables.count(name))
            name = base + std::to_string(++i);
        tab.Graph.Variables[name] = Editor::ScriptVar();
        tab.Graph.Dirty = true;
    }

    std::vector<std::string> names;
    std::vector<Editor::ScriptVar> vars;
    {
        std::lock_guard<std::mutex> lock(tab.Graph.VarsMutex);
        for (auto& [n, v] : tab.Graph.Variables) { names.push_back(n); vars.push_back(v); }
    }

    for (size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(names[i].c_str());
        char nbuf[64];
        strncpy(nbuf, names[i].c_str(), sizeof nbuf); nbuf[63] = 0;
        if (ImGui::InputText("##name", nbuf, sizeof nbuf)) {
            std::string old = names[i], neu = nbuf;
            if (!neu.empty() && neu != old) {
                std::lock_guard<std::mutex> lock(tab.Graph.VarsMutex);
                if (!tab.Graph.Variables.count(neu)) {
                    tab.Graph.Variables[neu] = tab.Graph.Variables[old];
                    tab.Graph.Variables.erase(old);
                    for (Editor::Node& n : tab.Graph.Nodes)
                        if ((n.Type == "GetVar" || n.Type == "SetVar") && n.Meta == old) {
                            n.Meta = neu;
                            n.Name = (n.Type == "GetVar" ? "Get " : "Set ") + neu;
                        }
                    tab.Graph.Dirty = true;
                }
            }
        }
        ImGui::SameLine();

        int cur = 0;
        for (int t = 0; t < IM_ARRAYSIZE(kVarTypes); ++t)
            if (kVarTypes[t] == vars[i].Type) cur = t;
        ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##type", kVarTypeNames[cur])) {
            for (int t = 0; t < IM_ARRAYSIZE(kVarTypeNames); ++t) {
                if (ImGui::Selectable(kVarTypeNames[t], t == cur)) {
                    std::lock_guard<std::mutex> lock(tab.Graph.VarsMutex);
                    tab.Graph.Variables[names[i]].Type = kVarTypes[t];
                    tab.Graph.Dirty = true;
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button("Get")) Nodes::GetVar(tab.Graph, names[i], vars[i].Type);
        ImGui::SameLine();
        if (ImGui::Button("Set")) Nodes::SetVar(tab.Graph, names[i], vars[i].Type);
        ImGui::SameLine();
        if (ImGui::Button("x")) {
            std::lock_guard<std::mutex> lock(tab.Graph.VarsMutex);
            tab.Graph.Variables.erase(names[i]);
            tab.Graph.Dirty = true;
        }
        ImGui::PopID();
    }
    ImGui::End();
}

static void RenderCustomNodesPanel() {
    ImGui::Begin("Custom Nodes");
    if (ImGui::Button("+ New Custom Node...", ImVec2(-1, 0)))
        CustomNodes::ShowCreate = true;

    std::vector<std::string> categories;
    for (const CustomNodeDef& d : CustomNodes::All()) {
        bool seen = false;
        for (const std::string& c : categories) if (c == d.Category) { seen = true; break; }
        if (!seen) categories.push_back(d.Category);
    }
    for (const std::string& cat : categories) {
        if (!ImGui::CollapsingHeader(cat.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        for (CustomNodeDef& d : CustomNodes::All()) {
            if (d.Category != cat) continue;
            ImGui::PushID(d.Name.c_str());
            if (ImGui::Button(d.Name.c_str(), ImVec2(-1, 0)) && !g_Tabs.empty())
                CustomNodes::SpawnInstance(g_Active->Graph, d);
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit"))
                CustomNodes::EditName = d.Name;
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) {
                PurgeCustomInstances(d.Name);
                CustomNodes::Delete(d.Name);
                IO::SaveCustomNodes();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

static void RenderCustomEditor() {
    if (CustomNodes::EditName.empty()) return;
    CustomNodeDef* def = CustomNodes::Find(CustomNodes::EditName);
    if (!def) { CustomNodes::EditName.clear(); return; }

    ImGui::SetNextWindowSize(ImVec2(1000, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin(("Custom Node: " + def->Name).c_str());
    {
        char cat[64];
        strncpy(cat, def->Category.c_str(), sizeof cat); cat[63] = 0;
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("Category", cat, sizeof cat)) {
            def->Category = cat;
            def->Body.Dirty = true;
        }
        ImGui::SameLine();
        if (!def->Body.FindNodeByType("CustomInput")) {
            if (ImGui::Button("+ Input Node")) Nodes::SpawnCustomIO(def->Body, "CustomInput");
        }
        ImGui::SameLine();
        if (!def->Body.FindNodeByType("CustomOutput")) {
            if (ImGui::Button("+ Output Node")) Nodes::SpawnCustomIO(def->Body, "CustomOutput");
        }
        ImGui::SameLine();
        if (ImGui::Button("Done")) {
            CustomNodes::EditName.clear();
            for (ScriptTab& tab : g_Tabs)
                CustomNodes::SyncInstances(tab.Graph);
            IO::SaveCustomNodes();
        }

        ImGui::BeginChild("toolbox", ImVec2(220, 0), true);
        Toolbox::Render(def->Body, true);
        ImGui::EndChild();
        ImGui::SameLine();

        if (!def->EdCtx) {
            def->EdCtx = ed::CreateEditor();
            ed::SetCurrentEditor(def->EdCtx);
            for (Editor::Node& n : def->Body.Nodes)
                ed::SetNodePosition(n.ID, n.Pos);
        } else {
            ed::SetCurrentEditor(def->EdCtx);
        }

        ImGui::BeginChild("canvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
        ed::Begin(def->Name.c_str(), ImVec2(0, 0));
        Editor::RenderNodes(def->Body);
        Editor::PollInput(def->Body);
        ed::End();
        ImGui::EndChild();
    }
    ImGui::End();
}

static void RenderCreateCustomPopup() {
    if (!CustomNodes::ShowCreate) return;
    ImGui::SetNextWindowSize(ImVec2(380, 140), ImGuiCond_FirstUseEver);
    ImGui::Begin("New Custom Node", &CustomNodes::ShowCreate, ImGuiWindowFlags_NoDocking);
    static char name[64] = "";
    static char category[64] = "Custom";
    ImGui::InputText("Name", name, sizeof name);
    ImGui::InputText("Category", category, sizeof category);
    if (ImGui::Button("Create") && name[0]) {
        if (CustomNodes::Create(name, category)) {
            IO::SaveCustomNodes();
            CustomNodes::EditName = name;
            strcpy_s(category, "Custom");
            name[0] = 0;
            CustomNodes::ShowCreate = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        CustomNodes::ShowCreate = false;
    ImGui::End();
}

static void RenderLoadPopup() {
    if (!g_ShowLoad) return;
    ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("Load Script", &g_ShowLoad, ImGuiWindowFlags_NoDocking);
    for (auto& entry : fs::directory_iterator(IO::ScriptsDir())) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        if (entry.path().filename().string() == "custom_nodes.json") continue;
        std::string stem = entry.path().stem().string();
        if (ImGui::Selectable(stem.c_str())) {
            StopTabHooks(*g_Active);
            if (IO::LoadGraph(g_Active->Graph, entry.path().string())) {
                g_Active->Name = stem;
                g_Active->Path = entry.path().string();
                ApplyNodePositions(*g_Active);
            }
            g_ShowLoad = false;
        }
    }
    ImGui::End();
}

static void RenderSaveAsPopup() {
    if (!g_ShowSaveAs) return;
    ImGui::SetNextWindowSize(ImVec2(380, 120), ImGuiCond_FirstUseEver);
    ImGui::Begin("Save Script As", &g_ShowSaveAs, ImGuiWindowFlags_NoDocking);
    ImGui::InputText("##name", g_SaveAsName, sizeof g_SaveAsName);
    if (ImGui::Button("Save") && g_SaveAsName[0])
        SaveActiveAs(g_SaveAsName);
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        g_ShowSaveAs = false;
    ImGui::End();
}

int RunApp()
{
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);

    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"UEEditor", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"UEEditor", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    //io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    IO::LoadCustomNodes();
    NewTab();
    g_Active = g_Tabs.begin();

    Exec::InitBackend();   // no-op in the standalone build

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        {
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            if (g_Active == g_Tabs.end() && !g_Tabs.empty())
                g_Active = g_Tabs.begin();

            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("New Script", "Ctrl+N")) NewTab();
                    if (ImGui::MenuItem("Load...", "Ctrl+O")) g_ShowLoad = true;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Save", "Ctrl+S", false, !g_Tabs.empty() && !g_Active->Path.empty())) {
                        if (IO::SaveGraph(g_Active->Graph, g_Active->Path)) g_Active->Graph.Dirty = false;
                    }
                    if (ImGui::MenuItem("Save As...", nullptr, false, !g_Tabs.empty()))
                        g_ShowSaveAs = true;
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Script", !g_Tabs.empty())) {
                    ScriptTab& tab = *g_Active;
                    bool playing = tab.Graph.Playing.load();
                    if (ImGui::MenuItem(playing ? "Pause" : "Play", nullptr, playing))
                        tab.Graph.Playing.store(!playing);
                    ImGui::TextDisabled("Script: %s", tab.Name.c_str());
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                ImGui::TextDisabled("Backend: %s", Exec::BackendStatus());
                ImGui::EndMainMenuBar();
            }

            ImGui::Begin("Node Editor");
            {
                if (!g_Tabs.empty()) {
                    ScriptTab& tab = *g_Active;
                    bool playing = tab.Graph.Playing.load();
                    if (ImGui::Button(playing ? "Pause" : "Play"))
                        tab.Graph.Playing.store(!playing);
                }

                // tab bar: ImGui owns selection; g_Active mirrors it. New tabs
                // are auto-selected via AutoSelectNewTabs.
                bool addPressed = false;
                ScriptTab* ctxTab = nullptr;
                float stripTop = ImGui::GetCursorScreenPos().y;
                if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_AutoSelectNewTabs)) {
                    for (auto it = g_Tabs.begin(); it != g_Tabs.end(); ++it) {
                        std::string label = it->Name + (it->Graph.Dirty ? " *" : "") + "###tab" + std::to_string((uintptr_t)&*it);
                        bool open = it->Open;
                        if (ImGui::BeginTabItem(label.c_str(), &open)) {
                            g_Active = it;
                            ImGui::BeginChild("##canvas", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
                            ed::SetCurrentEditor(it->Ctx);
                            ed::Begin(it->Name.c_str(), ImVec2(0, 0));
                            Editor::RenderNodes(it->Graph);
                            Editor::PollInput(it->Graph);
                            ed::End();
                            ImGui::EndChild();
                            ImGui::EndTabItem();
                        } else if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(1)) {
                            ctxTab = &*it;   // right-click an unselected tab header
                        }
                        if (!open) it->Open = false;
                    }
                    addPressed = ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing);
                    ImGui::EndTabBar();
                }
                float stripBottom = ImGui::GetCursorScreenPos().y;

                // right-click on the selected tab's header (hover can't be read
                // once its content was submitted, so hit-test the strip)
                if (!ctxTab && !g_Tabs.empty() && ImGui::IsWindowHovered()) {
                    ImVec2 m = ImGui::GetIO().MousePos;
                    if (m.y >= stripTop && m.y < stripBottom && ImGui::IsMouseReleased(1))
                        ctxTab = &*g_Active;
                }

                if (ctxTab) {
                    g_CtxTab = ctxTab;
                    ImGui::OpenPopup("tabctx");
                }
                if (ImGui::BeginPopup("tabctx")) {
                    ScriptTab& tab = *g_CtxTab;
                    char buf[128];
                    strncpy(buf, tab.Name.c_str(), sizeof buf); buf[sizeof buf - 1] = 0;
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::InputText("##rename", buf, sizeof buf)) {
                        std::string n = buf;
                        if (!n.empty() && n != tab.Name) {
                            tab.Name = n;
                            if (!tab.Path.empty())
                                tab.Path = (fs::path(IO::ScriptsDir()) / (n + ".json")).string();
                        }
                    }
                    ImGui::Separator();
                    bool playing = tab.Graph.Playing.load();
                    if (ImGui::MenuItem(playing ? "Pause" : "Play"))
                        tab.Graph.Playing.store(!playing);
                    if (ImGui::MenuItem("Save")) {
                        if (tab.Path.empty()) g_ShowSaveAs = true;
                        else if (IO::SaveGraph(tab.Graph, tab.Path)) { tab.Graph.Dirty = false; g_CtxTab = nullptr; ImGui::CloseCurrentPopup(); }
                    }
                    if (ImGui::MenuItem("Save As...")) g_ShowSaveAs = true;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Close Tab")) {
                        for (ScriptTab& t : g_Tabs)
                            if (&t == g_CtxTab) t.Open = false;
                        g_CtxTab = nullptr;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                for (auto it = g_Tabs.begin(); it != g_Tabs.end(); ) {
                    if (!it->Open) { CloseTab(it); it = g_Tabs.begin(); }
                    else ++it;
                }
                if (addPressed) NewTab();
            }
            ImGui::End();

            if (!g_Tabs.empty()) {
                RenderVariablesPanel(*g_Active);
                for (ScriptTab& tab : g_Tabs)
                    if (tab.Graph.Playing.load())
                        Exec::TickFrame(tab.Graph);
            }
            RenderCustomNodesPanel();

            RenderCustomEditor();
            RenderCreateCustomPopup();
            RenderLoadPopup();
            RenderSaveAsPopup();
            ImGui::Begin("Toolbox");
            if (!g_Tabs.empty())
                Toolbox::Render(g_Active->Graph);
            ImGui::End();
        }

        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    for (ScriptTab& tab : g_Tabs) {
        StopTabHooks(tab);
        ed::DestroyEditor(tab.Ctx);
    }
    for (CustomNodeDef& d : CustomNodes::All())
        if (d.EdCtx) ed::DestroyEditor(d.EdCtx);
    IO::SaveCustomNodes();
    g_Tabs.clear();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    IDXGIFactory* pSwapChainFactory;
    if (SUCCEEDED(g_pSwapChain->GetParent(IID_PPV_ARGS(&pSwapChainFactory))))
    {
        pSwapChainFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        pSwapChainFactory->Release();
    }

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
