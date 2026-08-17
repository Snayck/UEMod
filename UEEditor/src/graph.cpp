#include "graph.h"

using namespace Editor;

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

void RemoveLinksTouchingNode(Graph& g, const Node& node) {
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

void Editor::RenderNodes(Graph& g) {
    for (Node& n : g.Nodes) {
        ed::BeginNode(n.ID);
        ImGui::PushID(n.ID.AsPointer());

        ImGui::TextColored(n.Color, "%s", n.Name.c_str());
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
            if (IsLiteralType(p.Type) && !IsPinConnected(g, p.ID)) {
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
            ed::EndPin();
        }
        ImGui::EndGroup();

        ImGui::PopID();
        ed::EndNode();
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
            }
        }

        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId)) {
            if (ed::AcceptDeletedItem()) {
                auto nit = std::find_if(g.Nodes.begin(), g.Nodes.end(),
                    [&](const Node& n){ return n.ID == deletedNodeId; });
                if (nit != g.Nodes.end()) {
                    RemoveLinksTouchingNode(g, *nit);
                    g.Nodes.erase(nit);
                }
            }
        }
    }
    ed::EndDelete();
}
