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

bool IsValidConnection(Graph& g, ed::PinId aId, ed::PinId bId) {
    Pin* a = FindPin(g, aId);
    Pin* b = FindPin(g, bId);
    if (!a || !b)            return false;   // a pin didn't resolve
    if (a == b)              return false;   // pin to itself
    if (a->Node == b->Node)  return false;   // same node's own pins
    if (a->Kind == b->Kind)  return false;   // must be one Output + one Input
    if (a->Type != b->Type)  return false;   // types must match (Flow↔Flow, Float↔Float…)
    return true;
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

void Editor::RenderNodes(Graph& g) {
    for (Node& n : g.Nodes) {
        ed::BeginNode(n.ID);
        ImGui::PushID(n.ID.AsPointer());

        // --- title (colored) ---
        ImGui::TextColored(n.Color, "%s", n.Name.c_str());
        ImGui::Dummy(ImVec2(0, 4));

        // --- inputs column ---
        ImGui::BeginGroup();
        for (Pin& p : n.Inputs) {
            ed::BeginPin(p.ID, ed::PinKind::Input);
            DrawPinIcon(g, p);
            ImGui::SameLine();
            ImGui::TextUnformatted(p.Name.c_str());
            // inline literal text box for unconnected data inputs:
            if (p.Type != PinType::Flow && !IsPinConnected(g, p.ID)) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                char buf[128];
                strncpy(buf, p.DefaultValue.c_str(), sizeof buf); buf[127] = 0;
                if (ImGui::InputText("##lit", buf, sizeof buf)) {}
                   // p.DefaultValue = UEValue::MakeString(buf);  // parsed to real type at runtime
            }
            ed::EndPin();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0, 40); 

        ImGui::BeginGroup();
        for (Pin& p : n.Outputs) {
            ed::BeginPin(p.ID, ed::PinKind::Output);
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
    if (ed::BeginCreate()) { // user is dragging a new link
        ed::PinId startId, endId;
        if (ed::QueryNewLink(&startId, &endId)) {
            if (startId && endId) {
                if (IsValidConnection(g, startId, endId)) {
                    if (ed::AcceptNewItem()) { // true on the frame they release
                        g.Links.push_back(Link(g.NextId++, startId, endId));
                    }
                } else {
                    ed::RejectNewItem(ImColor(255,0,0), 2.0f);
                }
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) { // deleting node/link
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
                    RemoveLinksTouchingNode(g, *nit);  // clean up its wires first
                    g.Nodes.erase(nit);
                }
            }
        }
    }
    ed::EndDelete();
}