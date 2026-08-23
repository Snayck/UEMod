#include "graph.h"
#include "nodelogic.h"
#include "classcache.h"
#include <chrono>
#include <unordered_map>

using namespace Editor;

static double SteadyNowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double NanosToSec(int64_t ns) { return (double)ns * 1e-9; }

Pin* FindPin(Graph& g, ed::PinId id);

// ---- target-class resolution + pin dropdowns ---------------------------------

static std::unordered_map<uint32_t, const ClassInfo*> g_PinClassCache;

static const ClassInfo* ResolveTargetClass(Graph& g, Node& n) {
    Pin* target = nullptr;
    for (Pin& p : n.Inputs)
        if (p.Name == "Target") { target = &p; break; }
    if (!target) return nullptr;

    for (const Link& l : g.Links) {
        if (l.EndPinID != target->ID) continue;
        Pin* src = FindPin(g, l.StartPinID);
        if (!src || !src->Node) break;
        uint32_t key = (uint32_t)src->ID.Get();
        auto it = g_PinClassCache.find(key);
        if (it != g_PinClassCache.end()) return it->second;

        const ClassInfo* ci = nullptr;
        const std::string& t = src->Node->Type;
        if (t == "GetWorld") ci = ClassCache::Find("World");
        else ci = ClassCache::Get(Exec::ProbeInputClass(g, n, "Target"));
        if (ci) g_PinClassCache[key] = ci;
        return ci;
    }
    return ClassCache::Get(Exec::ProbeInputClass(g, n, "Target"));
}

static char g_ClassSearch[128] = "";

// combos can't live inside node windows (popups misplace); queue a request and
// open the popup at canvas level after ed::End()
struct ComboReq {
    Graph*  GraphPtr;
    ed::PinId PinId;
    bool    Field;
    bool    Opened = false;
};
static std::vector<ComboReq> g_ComboReqs;

static void RenderComboList(Graph& g, Node& n, Pin& p, bool field) {
    const ClassInfo* ci = ResolveTargetClass(g, n);
    if (!ci) {
        ImGui::TextDisabled("target class unknown - type a class:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##cs", "class name", g_ClassSearch, sizeof g_ClassSearch);
        if (g_ClassSearch[0])
            ci = ClassCache::Find(g_ClassSearch);
    }
    if (!ci) return;
    for (const ClassLevel& lvl : ci->Levels) {
        if (field && lvl.Fields.empty()) continue;
        if (!field && lvl.Functions.empty()) continue;
        ImGui::SeparatorText(lvl.ClassName.c_str());
        if (field) {
            for (const FieldDesc& f : lvl.Fields) {
                bool sel = p.DefaultValue == f.Name;
                if (ImGui::Selectable((f.Name + "##" + lvl.ClassName).c_str(), sel)) {
                    p.DefaultValue = f.Name;
                    g.Dirty = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        } else {
            for (const FuncDesc& f : lvl.Functions) {
                bool sel = p.DefaultValue == f.Name || p.DefaultValue == f.FullName;
                if (ImGui::Selectable((f.Name + "##" + lvl.ClassName).c_str(), sel)) {
                    p.DefaultValue = f.FullName;
                    g.Dirty = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }
}

static void DrawComboButton(Graph& g, Node& n, Pin& p, bool field) {
    std::string label = p.DefaultValue.empty()
        ? (field ? "<field>" : "<function>") : p.DefaultValue;
    if (ImGui::SmallButton((label + "##cmb").c_str())) {
        ComboReq r;
        r.GraphPtr = &g;
        r.PinId = p.ID;
        r.Field = field;
        g_ComboReqs.push_back(r);
    }
}

void Editor::RenderDeferredCombos(Graph& g) {
    for (auto it = g_ComboReqs.begin(); it != g_ComboReqs.end(); ) {
        if (it->GraphPtr != &g) { ++it; continue; }
        Pin* pin = FindPin(g, it->PinId);
        Node* node = pin ? pin->Node : nullptr;
        if (!node) { it = g_ComboReqs.erase(it); continue; }

        ImGui::PushID((int)it->PinId.Get());
        if (!it->Opened) {
            ImGui::OpenPopup("##dd");   // latches to current mouse pos
            it->Opened = true;
        }
        if (ImGui::BeginPopup("##dd")) {
            RenderComboList(g, *node, *pin, it->Field);
            ImGui::EndPopup();
            ++it;
        } else {
            it = g_ComboReqs.erase(it);
        }
        ImGui::PopID();
    }
}

static bool IsFieldComboPin(Node& n, Pin& p) {
    return p.Name == "Field" && (n.Type == "GetValue" || n.Type == "SetValue");
}

static bool IsFunctionComboPin(Node& n, Pin& p) {
    return p.Name == "Function" && n.Type == "CallFunction";
}

Pin* FindPin(Graph& g, ed::PinId id) {
    if (!id) return nullptr;
    for (Node& n : g.Nodes) {
        for (Pin& p : n.Inputs)  if (p.ID == id) return &p;
        for (Pin& p : n.Outputs) if (p.ID == id) return &p;
    }
    return nullptr;
}

static bool TypesCompatible(PinType out, PinType in) {
    if (out == in) return true;
    if (out == PinType::Flow || in == PinType::Flow) return false;
    if (out == PinType::Any || in == PinType::Any) return true;
    if (out == PinType::Int && in == PinType::Float) return true;
    if (out == PinType::Bool && in == PinType::Int) return true;
    if (out == PinType::Name && in == PinType::String) return true;
    return false;
}

bool IsValidConnection(Graph& g, ed::PinId aId, ed::PinId bId) {
    Pin* a = FindPin(g, aId);
    Pin* b = FindPin(g, bId);
    if (!a || !b)            return false;
    if (a == b)              return false;
    if (a->Node == b->Node)  return false;
    if (a->Kind == b->Kind)  return false;
    Pin* out = a->Kind == PinKind::Output ? a : b;
    Pin* in  = a->Kind == PinKind::Output ? b : a;
    return TypesCompatible(out->Type, in->Type);
}

void Editor::RemoveLinksTouchingNode(Graph& g, const Node& node) {
    auto belongsToNode = [&](ed::PinId pin) {
        for (const Pin& p : node.Inputs)  if (p.ID == pin) return true;
        for (const Pin& p : node.Outputs) if (p.ID == pin) return true;
        return false;
    };
    g.Links.erase(
        std::remove_if(g.Links.begin(), g.Links.end(),
            [&](const Link& l){ return belongsToNode(l.StartPinID) || belongsToNode(l.EndPinID); }),
        g.Links.end());
}

static bool IsLiteralType(PinType t) {
    return t == PinType::Float || t == PinType::Int || t == PinType::Bool
        || t == PinType::String || t == PinType::Name || t == PinType::Any;
}

static void RemoveLinksTouchingPin(Graph& g, ed::PinId pin) {
    g.Links.erase(
        std::remove_if(g.Links.begin(), g.Links.end(),
            [&](const Link& l){ return l.StartPinID == pin || l.EndPinID == pin; }),
        g.Links.end());
}

static void DrawTypeSelector(Graph& g, Pin& p) {
    static const char*   kNames[] = { "Float", "Int", "Bool", "String", "Name" };
    static const PinType kTypes[] = { PinType::Float, PinType::Int, PinType::Bool, PinType::String, PinType::Name };
    int cur = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kTypes); ++i)
        if (kTypes[i] == p.Type) cur = i;

    ImGui::PushID(p.ID.AsPointer());
    char btnLabel[32];
    snprintf(btnLabel, sizeof btnLabel, "%s##btn", kNames[cur]);
    if (ImGui::Button(btnLabel, ImVec2(90, 0)))
        p.SelectorOpen = !p.SelectorOpen;
    if (p.SelectorOpen) {
        for (int i = 0; i < IM_ARRAYSIZE(kTypes); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(kNames[i], i == cur, 0, ImVec2(90, 0))) {
                if (kTypes[i] != p.Type) {
                    p.Type = kTypes[i];
                    RemoveLinksTouchingPin(g, p.ID);
                }
                p.SelectorOpen = false;
            }
            ImGui::PopID();
        }
    }
    ImGui::SetNextItemWidth(90);
    char buf[128];
    strncpy(buf, p.DefaultValue.c_str(), sizeof buf); buf[127] = 0;
    if (ImGui::InputText("##val", buf, sizeof buf))
        p.DefaultValue = buf;
    ImGui::PopID();
}

// cycle a pin's type; drops links when the type changes
static void TypeCycleButton(Graph& g, Pin& p) {
    static const PinType kCycle[] = { PinType::Bool, PinType::Int, PinType::Float, PinType::String,
                                      PinType::Name, PinType::Object, PinType::Struct, PinType::Array, PinType::Any };
    static const char* kCycleNames[] = { "Bool", "Int", "Float", "String", "Name", "Object", "Struct", "Array", "Any" };
    int cur = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kCycle); ++i)
        if (kCycle[i] == p.Type) cur = i;
    ImGui::PushID(p.ID.AsPointer());
    if (ImGui::Button(kCycleNames[cur], ImVec2(60, 0))) {
        p.Type = kCycle[(cur + 1) % IM_ARRAYSIZE(kCycle)];
        RemoveLinksTouchingPin(g, p.ID);
        g.Dirty = true;
    }
    ImGui::PopID();
}

// editable pin row for the CustomInput / CustomOutput interface nodes
static void DrawCustomIOPin(Graph& g, Pin& p) {
    ImGui::PushID(p.ID.AsPointer());
    ImGui::SetNextItemWidth(110);
    char buf[64];
    strncpy(buf, p.Name.c_str(), sizeof buf); buf[63] = 0;
    if (ImGui::InputText("##nm", buf, sizeof buf)) {
        p.Name = buf;
        g.Dirty = true;
    }
    ImGui::SameLine();
    TypeCycleButton(g, p);
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        RemoveLinksTouchingPin(g, p.ID);
        p.Name = "\x01__del";
        g.Dirty = true;
    }
    ImGui::PopID();
}

void Editor::RenderNodes(Graph& g) {
    // drop pins flagged for deletion (CustomInput/Output "x" buttons)
    for (Node& n : g.Nodes) {
        auto drop = [](std::vector<Pin>& pins) {
            pins.erase(std::remove_if(pins.begin(), pins.end(),
                [](const Pin& p) { return p.Name == "\x01__del"; }), pins.end());
        };
        drop(n.Inputs);
        drop(n.Outputs);
    }

    for (Node& n : g.Nodes) {
        float errAlpha = 0.0f;
        int64_t errNs = n.ErrorNs.load(std::memory_order_relaxed);
        if (errNs > 0) {
            double age = SteadyNowSec() - NanosToSec(errNs);
            if (age >= 0.0 && age < 2.0)
                errAlpha = (age < 1.0) ? 1.0f : (float)(2.0 - age);
        }
        if (errAlpha > 0.0f) {
            ed::PushStyleColor(ed::StyleColor_NodeBorder,
                ImColor(255, 48, 48, (int)(255.0f * errAlpha)).Value);
            ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 3.0f);
        }

        ed::BeginNode(n.ID);
        ImGui::PushID(n.ID.AsPointer());

        n.Pos = ed::GetNodePosition(n.ID);

        ImGui::TextColored(n.Color, "%s", n.Name.c_str());
        if (n.Type == "ScriptStart") {
            ImGui::SameLine();
            if (ImGui::SmallButton("Run"))
                Exec::RunScriptNode(g, n);
        }
        if (n.Type == "PreHook" || n.Type == "PostHook") {
            ImGui::SameLine();
            bool on = n.HookHandle > 0;
            ImGui::PushStyleColor(ImGuiCol_Text, on ? ImColor(80, 220, 100).Value : ImColor(150, 150, 150).Value);
            ImGui::TextUnformatted(on ? "on" : "off");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton(on ? "Stop" : "Start"))
                on ? Exec::StopHook(n) : (void)Exec::StartHook(g, n);
        }
        ImGui::Dummy(ImVec2(0, 4));

        for (Pin& p : n.Outputs)
            if (p.Selectable) DrawTypeSelector(g, p);

        ImGui::BeginGroup();
        for (Pin& p : n.Inputs) {
            ed::BeginPin(p.ID, ed::PinKind::Input);
            ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
            ed::PinPivotSize(ImVec2(0, 0));
            DrawPinIcon(g, p);
            ImGui::SameLine();
            ImGui::TextUnformatted(p.Name.c_str());
            if (n.Type == "CustomOutput" && p.Type != PinType::Flow) {
                ImGui::SameLine();
                DrawCustomIOPin(g, p);
            } else if (!IsPinConnected(g, p.ID) && IsFieldComboPin(n, p)) {
                ImGui::SameLine();
                ImGui::PushID(p.ID.AsPointer());
                DrawComboButton(g, n, p, true);
                ImGui::PopID();
            } else if (!IsPinConnected(g, p.ID) && IsFunctionComboPin(n, p)) {
                ImGui::SameLine();
                ImGui::PushID(p.ID.AsPointer());
                DrawComboButton(g, n, p, false);
                ImGui::PopID();
            } else if (IsLiteralType(p.Type) && !IsPinConnected(g, p.ID)) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                char buf[128];
                strncpy(buf, p.DefaultValue.c_str(), sizeof buf); buf[127] = 0;
                ImGui::PushID(p.ID.AsPointer());
                if (ImGui::InputText("##lit", buf, sizeof buf))
                    p.DefaultValue = buf;
                ImGui::PopID();
            }
            ed::EndPin();
        }
        ImGui::EndGroup();
        ImGui::SameLine();

        float outStartX = ImGui::GetCursorPosX();
        float maxOutW = 0.0f;
        for (Pin& p : n.Outputs) {
            float w = ImGui::CalcTextSize(p.Name.c_str()).x;
            if (w > maxOutW) maxOutW = w;
        }

        ImGui::BeginGroup();
        for (Pin& p : n.Outputs) {
            float tw = ImGui::CalcTextSize(p.Name.c_str()).x;
            ImGui::SetCursorPosX(outStartX + (maxOutW - tw));
            ed::BeginPin(p.ID, ed::PinKind::Output);
            ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
            ed::PinPivotSize(ImVec2(0, 0));
            ImGui::TextUnformatted(p.Name.c_str());
            ImGui::SameLine();
            DrawPinIcon(g, p);
            if (n.Type == "CustomInput" && p.Type != PinType::Flow) {
                ImGui::SameLine();
                DrawCustomIOPin(g, p);
            }
            ed::EndPin();
        }
        if (n.Type == "CustomInput" && ImGui::SmallButton("+ Output")) {
            Pin& p = n.Outputs.emplace_back(g.GetNextId(), "In", PinType::Any);
            p.Node = &n; p.Kind = PinKind::Output;
            g.Dirty = true;
        }
        if (n.Type == "CustomOutput" && ImGui::SmallButton("+ Input")) {
            Pin& p = n.Inputs.emplace_back(g.GetNextId(), "Out", PinType::Any);
            p.Node = &n; p.Kind = PinKind::Input;
            g.Dirty = true;
        }
        ImGui::EndGroup();

        ImGui::PopID();
        ed::EndNode();

        if (errAlpha > 0.0f) {
            ed::PopStyleVar();
            ed::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", n.Error);
        }
    }
    for (Link& l : g.Links)
        ed::Link(l.ID, l.StartPinID, l.EndPinID);
}

void Editor::PollInput(Graph& g) {
    if (ed::BeginCreate()) {
        ed::PinId startId, endId;
        if (ed::QueryNewLink(&startId, &endId)) {
            if (startId && endId) {
                if (IsValidConnection(g, startId, endId)) {
                    if (ed::AcceptNewItem()) {
                        g.Links.push_back(Link(g.NextId++, startId, endId));
                        g_PinClassCache.clear();
                        g.Dirty = true;
                    }
                } else {
                    ed::RejectNewItem(ImColor(255,0,0), 2.0f);
                }
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId)) {
            if (ed::AcceptDeletedItem()) {
                auto it = std::find_if(g.Links.begin(), g.Links.end(),
                    [&](const Link& l){ return l.ID == deletedLinkId; });
                if (it != g.Links.end())
                    g.Links.erase(it);
                g.Dirty = true;
            }
        }

        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId)) {
            if (ed::AcceptDeletedItem()) {
                auto nit = std::find_if(g.Nodes.begin(), g.Nodes.end(),
                    [&](const Node& n){ return n.ID == deletedNodeId; });
                if (nit != g.Nodes.end()) {
                    Exec::StopHook(*nit);
                    RemoveLinksTouchingNode(g, *nit);
                    g.Nodes.erase(nit);
                    g.Dirty = true;
                }
            }
        }
    }
    ed::EndDelete();

    // ---- context menus ---------------------------------------------------------
    // Show*ContextMenu returns true only on the frame the right-click happens:
    // open the popup then, and submit it every frame below while it stays open.
    static ed::NodeId ctxNode;
    static ed::LinkId ctxLink;
    static ed::PinId  ctxPin;
    bool wantNode = ed::ShowNodeContextMenu(&ctxNode);
    bool wantLink = ed::ShowLinkContextMenu(&ctxLink);
    bool wantPin  = ed::ShowPinContextMenu(&ctxPin);
    if (wantNode || wantLink || wantPin) {
        ed::Suspend();
        if (wantNode) ImGui::OpenPopup("##nodectx");
        if (wantLink) ImGui::OpenPopup("##linkctx");
        if (wantPin)  ImGui::OpenPopup("##pinctx");
        ed::Resume();
    }

    ed::Suspend();
    if (ImGui::BeginPopup("##nodectx")) {
        auto nit = std::find_if(g.Nodes.begin(), g.Nodes.end(),
            [&](const Node& n){ return n.ID == ctxNode; });
        if (nit != g.Nodes.end()) {
            Node& n = *nit;
            if (ImGui::MenuItem("Duplicate")) {
                Node& copy = g.Nodes.emplace_back(g.GetNextId(), n.Type.c_str(), n.Color);
                copy.Name = n.Name;
                copy.Meta = n.Meta;
                copy.Pos = ImVec2(n.Pos.x + 48, n.Pos.y + 48);
                for (const Pin& p : n.Inputs)
                    copy.Inputs.emplace_back(g.GetNextId(), p.Name.c_str(), p.Type).DefaultValue = p.DefaultValue;
                for (const Pin& p : n.Outputs)
                    copy.Outputs.emplace_back(g.GetNextId(), p.Name.c_str(), p.Type).DefaultValue = p.DefaultValue;
                for (Pin& p : copy.Inputs)  { p.Node = &copy; p.Kind = PinKind::Input; }
                for (Pin& p : copy.Outputs) { p.Node = &copy; p.Kind = PinKind::Output; }
                ed::SetNodePosition(copy.ID, copy.Pos);
                g.Dirty = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                Exec::StopHook(n);
                RemoveLinksTouchingNode(g, n);
                g.Nodes.erase(nit);
                g.Dirty = true;
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##linkctx")) {
        if (ImGui::MenuItem("Delete Link")) {
            auto it = std::find_if(g.Links.begin(), g.Links.end(),
                [&](const Link& l){ return l.ID == ctxLink; });
            if (it != g.Links.end()) {
                g.Links.erase(it);
                g.Dirty = true;
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##pinctx")) {
        if (ImGui::MenuItem("Disconnect"))
            RemoveLinksTouchingPin(g, ctxPin);
        ImGui::EndPopup();
    }
    ed::Resume();
}
