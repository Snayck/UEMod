#include "objectexplorer.h"
#include "imgui.h"

#ifndef UEEDITOR_WITH_BACKEND

namespace ObjectExplorer {
    void Render() {
        if (!ImGui::Begin("Object Explorer")) { ImGui::End(); return; }
        ImGui::TextDisabled("backend inactive (standalone)");
        ImGui::End();
    }
    void FlushSpawns(Editor::Graph&) {}
}

#else

#include "UEHook.h"
#include "classcache.h"
#include "nodelogic.h"
#include "nodes.h"
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ObjectExplorer {

namespace {

char g_ClassFilter[128] = "";
bool g_IncludeDefaults = false;

struct Instance { std::string Name; void* Ptr; };
std::vector<Instance> g_Results;

void* g_Selected = nullptr;
std::string g_SelectedName;
std::vector<void*> g_History;

enum class SpawnKind { GetNode, SetNode, PreHook, PostHook };
struct SpawnReq { SpawnKind Kind; std::string Field; std::string Function; };
std::vector<SpawnReq> g_SpawnReqs;

std::string g_EditPath;
char g_EditBuf[256] = "";

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t g_LastTickNs = 0;
bool TickDue() {
    int64_t now = NowNs();
    if (now - g_LastTickNs < 100000000LL) return false;
    g_LastTickNs = now;
    return true;
}

void Select(void* obj) {
    if (!obj) return;
    if (g_Selected && g_Selected != obj) g_History.push_back(g_Selected);
    g_Selected = obj;
    g_SelectedName = UEObject(obj).GetFullName();
}

void Search() {
    g_Results.clear();
    g_Selected = nullptr;
    g_History.clear();
    if (!UE::IsInitialized()) return;
    for (UEObject& o : UE::GetObjectsOfClass(g_ClassFilter, g_IncludeDefaults)) {
        if (!o) continue;
        g_Results.push_back({ o.GetName(), o.GetAddress() });
    }
}

bool IsEditableType(const std::string& tn) {
    return tn == "Bool" || tn == "Float" || tn == "Double" || tn == "Byte"
        || tn == "Str" || tn == "Name"
        || tn.rfind("Int", 0) == 0 || tn.rfind("UInt", 0) == 0;
}

bool IsWriteablePath(const std::string& path) {
    return path.find('[') == std::string::npos
        && path.find('{') == std::string::npos
        && path.find(".k") == std::string::npos
        && path.find(".v") == std::string::npos;
}

void RenderValue(const UEValue& v, const std::string& path, void* root);

struct RowCtx {
    std::string Path;
    bool MenuHasNodes = false;
    std::string NodeField;
    std::string FnName;
    bool Editable = false;
    std::string EditInit;
};

void RowEnd(RowCtx& rc, const ImVec2& rowMin) {
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 rowMax(cursor.x + ImGui::GetContentRegionAvail().x,
                  rowMin.y + ImGui::GetTextLineHeight() + 2.0f);

    bool hovered = ImGui::IsMouseHoveringRect(rowMin, rowMax) && ImGui::IsWindowHovered();
    if (hovered)
        ImGui::GetWindowDrawList()->AddRectFilled(rowMin, rowMax, IM_COL32(90, 120, 200, 40));

    if (hovered && rc.Editable && ImGui::IsMouseDoubleClicked(0)) {
        g_EditPath = rc.Path;
        strncpy(g_EditBuf, rc.EditInit.c_str(), sizeof g_EditBuf - 1);
        g_EditBuf[sizeof g_EditBuf - 1] = 0;
    }
    if (hovered && ImGui::IsMouseClicked(1))
        ImGui::OpenPopup("rowctx");
    if (ImGui::BeginPopup("rowctx")) {
        if (rc.MenuHasNodes) {
            if (ImGui::MenuItem("Add Get Value node"))
                g_SpawnReqs.push_back({ SpawnKind::GetNode, rc.NodeField, "" });
            if (ImGui::MenuItem("Add Set Value node"))
                g_SpawnReqs.push_back({ SpawnKind::SetNode, rc.NodeField, "" });
        } else {
            if (ImGui::MenuItem("Add PreHook"))
                g_SpawnReqs.push_back({ SpawnKind::PreHook, "", rc.FnName });
            if (ImGui::MenuItem("Add PostHook"))
                g_SpawnReqs.push_back({ SpawnKind::PostHook, "", rc.FnName });
        }
        ImGui::EndPopup();
    }
}

void RenderFieldRow(const FieldDesc& f, const UEValue& v, const std::string& path, void* root) {
    ImGui::PushID(path.c_str());
    ImVec2 rowMin = ImGui::GetCursorScreenPos();

    ImGui::TextColored(ImColor(120, 170, 255), "%s", f.TypeName.c_str());
    ImGui::SameLine();
    ImGui::TextColored(ImColor(255, 190, 120), "%s", f.Name.c_str());
    ImGui::SameLine();

    bool editable = root && IsEditableType(f.TypeName) && IsWriteablePath(path);
    if (editable && g_EditPath == path) {
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputText("##edit", g_EditBuf, sizeof g_EditBuf,
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            Exec::ApplyStringToField(root, path, g_EditBuf);
            g_EditPath.clear();
        }
        if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0))
            g_EditPath.clear();
    } else {
        RenderValue(v, path, root);
    }

    RowCtx rc;
    rc.Path = path;
    rc.MenuHasNodes = true;
    rc.NodeField = f.Name;
    rc.Editable = editable;
    rc.EditInit = v.ToString();
    RowEnd(rc, rowMin);
    ImGui::PopID();
}

void RenderFunctionRow(const FuncDesc& fn) {
    ImGui::PushID(fn.FullName.c_str());
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    ImGui::TextColored(ImColor(120, 170, 255), "%s()", fn.Name.c_str());
    RowCtx rc;
    rc.FnName = fn.FullName;
    RowEnd(rc, rowMin);
    ImGui::PopID();
}

void RenderMembers(UEStructRef ref, const std::string& path, void* root) {
    if (!ref) return;
    for (const UEProperty& p : ref.GetType().GetAllProperties()) {
        FieldDesc f;
        f.Name = p.GetName();
        f.TypeName = ClassCache::TrimTypeName(p.GetTypeName());
        RenderFieldRow(f, ref.GetValue(p.GetName()), path + "." + f.Name, root);
    }
}

void RenderValue(const UEValue& v, const std::string& path, void* root) {
    switch (v.Kind) {
    case UEValueKind::None:
        ImGui::TextDisabled("?");
        break;
    case UEValueKind::Struct:
        if (ImGui::TreeNode("struct")) {
            RenderMembers(UEStructRef(v), path, root);
            ImGui::TreePop();
        }
        break;
    case UEValueKind::Array: {
        UEArrayRef arr(v);
        if (!arr) { ImGui::TextDisabled("[?]"); break; }
        ImGui::TextDisabled("[%d]", (int)arr.Num());
        ImGui::SameLine();
        if (!ImGui::TreeNode("elems")) break;
        int n = arr.Num() < 200 ? arr.Num() : 200;
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            ImGui::Bullet();
            ImGui::SameLine();
            RenderValue(arr.At(i), path + "[" + std::to_string(i) + "]", root);
            ImGui::PopID();
        }
        if (arr.Num() > 200) ImGui::TextDisabled("+%d more", (int)(arr.Num() - 200));
        ImGui::TreePop();
        break;
    }
    case UEValueKind::Set: {
        UESetRef s(v);
        if (!s) { ImGui::TextDisabled("set{}"); break; }
        ImGui::TextDisabled("{%d}", (int)s.Num());
        ImGui::SameLine();
        if (!ImGui::TreeNode("elems")) break;
        int n = s.Num() < 200 ? s.Num() : 200;
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            ImGui::Bullet();
            ImGui::SameLine();
            RenderValue(s.At(i), path + "{" + std::to_string(i) + "}", root);
            ImGui::PopID();
        }
        if (s.Num() > 200) ImGui::TextDisabled("+%d more", (int)(s.Num() - 200));
        ImGui::TreePop();
        break;
    }
    case UEValueKind::Map: {
        UEMapRef m(v);
        if (!m) { ImGui::TextDisabled("map{}"); break; }
        ImGui::TextDisabled("{%d}", (int)m.Num());
        ImGui::SameLine();
        if (!ImGui::TreeNode("elems")) break;
        int n = m.Num() < 200 ? m.Num() : 200;
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            ImGui::Bullet();
            ImGui::SameLine();
            RenderValue(m.KeyAt(i), path + ".k", root);
            ImGui::SameLine();
            ImGui::TextUnformatted("->");
            ImGui::SameLine();
            RenderValue(m.ValueAt(i), path + ".v", root);
            ImGui::PopID();
        }
        if (m.Num() > 200) ImGui::TextDisabled("+%d more", (int)(m.Num() - 200));
        ImGui::TreePop();
        break;
    }
    case UEValueKind::Object:
        if (!v.Ptr) { ImGui::TextUnformatted("null"); break; }
        if (ImGui::SmallButton(UEObject(v.Ptr).GetName().c_str()))
            Select(v.Ptr);
        break;
    default:
        ImGui::TextUnformatted(v.ToString().c_str());
        break;
    }
}

} // namespace

void FlushSpawns(Editor::Graph& g) {
    for (const SpawnReq& r : g_SpawnReqs) {
        switch (r.Kind) {
        case SpawnKind::GetNode:  Nodes::GetValue(g, r.Field); break;
        case SpawnKind::SetNode:  Nodes::SetValue(g, r.Field); break;
        case SpawnKind::PreHook:  Nodes::PreHook(g, r.Function); break;
        case SpawnKind::PostHook: Nodes::PostHook(g, r.Function); break;
        }
        g.Dirty = true;
    }
    g_SpawnReqs.clear();
}

void Render() {
    if (!ImGui::Begin("Object Explorer")) { ImGui::End(); return; }

    if (!UE::IsInitialized()) {
        ImGui::TextDisabled("backend %s", Exec::BackendStatus());
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(220);
    if (ImGui::InputTextWithHint("##class", "class name", g_ClassFilter, sizeof g_ClassFilter,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
        Search();
    ImGui::SameLine();
    if (ImGui::Button("Search")) Search();
    ImGui::SameLine();
    ImGui::Checkbox("defaults", &g_IncludeDefaults);
    ImGui::SameLine();
    ImGui::TextDisabled("%d objects", (int)g_Results.size());

    ImGui::Separator();

    ImGui::BeginChild("##list", ImVec2(260, 0), true);
    for (size_t i = 0; i < g_Results.size(); ++i) {
        ImGui::PushID((int)i);
        bool sel = g_Selected == g_Results[i].Ptr;
        if (ImGui::Selectable(g_Results[i].Name.c_str(), sel))
            Select(g_Results[i].Ptr);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##detail", ImVec2(0, 0), true);

    if (g_Selected) {
        UEObject obj(g_Selected);
        if (!obj) {
            ImGui::TextDisabled("object no longer valid");
        } else {
            if (!g_History.empty() && ImGui::SmallButton("< Back")) {
                void* prev = g_History.back();
                g_History.pop_back();
                g_Selected = prev;
                g_SelectedName = UEObject(prev).GetFullName();
            }
            char full[512];
            strncpy(full, g_SelectedName.c_str(), sizeof full); full[sizeof full - 1] = 0;
            ImGui::PushItemWidth(-1);
            ImGui::InputText("##full", full, sizeof full, ImGuiInputTextFlags_ReadOnly);
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 4));

            const ClassInfo* ci = ClassCache::Get(obj.GetClass().GetAddress());
            if (ci) {
                bool tick = TickDue();
                (void)tick;
                for (const ClassLevel& lvl : ci->Levels) {
                    if (lvl.Fields.empty() && lvl.Functions.empty()) {
                        ImGui::TextDisabled("%s (no members)", lvl.ClassName.c_str());
                        continue;
                    }
                    if (ImGui::TreeNodeEx(lvl.ClassName.c_str(), 0, "%s", lvl.ClassName.c_str())) {
                        for (const FieldDesc& f : lvl.Fields)
                            RenderFieldRow(f, obj.GetValue(f.Name), f.Name, g_Selected);
                        if (!lvl.Fields.empty() && !lvl.Functions.empty())
                            ImGui::Separator();
                        for (const FuncDesc& fn : lvl.Functions)
                            RenderFunctionRow(fn);
                        ImGui::TreePop();
                    }
                }
            }
        }
    } else {
        ImGui::TextDisabled("search a class and select an object");
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace ObjectExplorer

#endif
