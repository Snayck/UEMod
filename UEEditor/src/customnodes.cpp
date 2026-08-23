#include "customnodes.h"
#include "nodes.h"
#include <algorithm>

using namespace Editor;

namespace {
    std::vector<CustomNodeDef>& Registry() {
        static std::vector<CustomNodeDef> all;
        return all;
    }

    const char* kInputType  = "CustomInput";
    const char* kOutputType = "CustomOutput";
}

namespace CustomNodes {

bool ShowCreate = false;
std::string EditName;

std::vector<CustomNodeDef>& All() { return Registry(); }

CustomNodeDef* Find(const std::string& name) {
    for (CustomNodeDef& d : Registry())
        if (d.Name == name) return &d;
    return nullptr;
}

CustomNodeDef* Create(const std::string& name, const std::string& category) {
    if (name.empty() || Find(name)) return nullptr;
    CustomNodeDef d;
    d.Name = name;
    d.Category = category.empty() ? "Custom" : category;
    Nodes::SpawnCustomIO(d.Body, kInputType);
    Nodes::SpawnCustomIO(d.Body, kOutputType);
    Registry().push_back(std::move(d));
    return &Registry().back();
}

void Delete(const std::string& name) {
    auto& all = Registry();
    all.erase(std::remove_if(all.begin(), all.end(),
        [&](const CustomNodeDef& d) { return d.Name == name; }), all.end());
}

void Interface(const CustomNodeDef& def,
               std::vector<CustomPinDesc>& inputs,
               std::vector<CustomPinDesc>& outputs) {
    inputs.clear(); outputs.clear();
    Graph& body = const_cast<Graph&>(def.Body);
    if (Node* in = body.FindNodeByType(kInputType))
        for (const Pin& p : in->Outputs)
            if (p.Type != PinType::Flow)
                inputs.push_back({ p.Name, p.Type });
    if (Node* out = body.FindNodeByType(kOutputType))
        for (const Pin& p : out->Inputs)
            if (p.Type != PinType::Flow)
                outputs.push_back({ p.Name, p.Type });
}

Node* SpawnInstance(Graph& g, const CustomNodeDef& def) {
    Node* n = &g.Nodes.emplace_back(g.GetNextId(), def.Name.c_str(), ImColor(255, 200, 100));
    n->Inputs.emplace_back(g.GetNextId(), "", PinType::Flow);
    n->Outputs.emplace_back(g.GetNextId(), "", PinType::Flow);
    std::vector<CustomPinDesc> ins, outs;
    Interface(def, ins, outs);
    for (const CustomPinDesc& d : ins)
        n->Inputs.emplace_back(g.GetNextId(), d.Name.c_str(), d.Type);
    for (const CustomPinDesc& d : outs)
        n->Outputs.emplace_back(g.GetNextId(), d.Name.c_str(), d.Type);
    for (Pin& p : n->Inputs)  { p.Node = n; p.Kind = PinKind::Input; }
    for (Pin& p : n->Outputs) { p.Node = n; p.Kind = PinKind::Output; }
    g.Dirty = true;
    return n;
}

static void RebuildPins(Graph& g, Node& n, const std::vector<CustomPinDesc>& descs,
                        PinKind kind, const char* execName) {
    std::vector<Pin>& pins = kind == PinKind::Input ? n.Inputs : n.Outputs;

    std::vector<ed::PinId> oldIds;
    for (const Pin& p : pins) oldIds.push_back(p.ID);

    // keep pin IDs of surviving pins so links stay valid
    std::vector<Pin> rebuilt;
    if (execName) {
        bool hasExec = false;
        for (const Pin& p : pins)
            if (p.Type == PinType::Flow) { hasExec = true; break; }
        if (!hasExec) {
            pins.emplace_back(g.GetNextId(), execName, PinType::Flow);
        }
    }
    for (const Pin& p : pins)
        if (p.Type == PinType::Flow)
            rebuilt.push_back(p);   // exec pins survive as-is

    for (const CustomPinDesc& d : descs) {
        Pin* existing = nullptr;
        for (Pin& p : rebuilt)
            if (p.Type != PinType::Flow && p.Name == d.Name && p.Type == d.Type && !existing)
                existing = &p;
        if (existing) continue;
        rebuilt.emplace_back(g.GetNextId(), d.Name.c_str(), d.Type);
    }
    // drop links touching this node's pins that no longer exist
    auto wasMine = [&](ed::PinId id) {
        for (ed::PinId o : oldIds) if (o == id) return true;
        return false;
    };
    auto stillThere = [&](ed::PinId id) {
        for (const Pin& p : rebuilt) if (p.ID == id) return true;
        return false;
    };
    g.Links.erase(std::remove_if(g.Links.begin(), g.Links.end(),
        [&](const Link& l) {
            return (wasMine(l.StartPinID) && !stillThere(l.StartPinID))
                || (wasMine(l.EndPinID)   && !stillThere(l.EndPinID));
        }), g.Links.end());

    pins = rebuilt;
    for (Pin& p : pins) { p.Node = &n; p.Kind = kind; }
}

void SyncInstances(Graph& g) {
    std::vector<CustomPinDesc> ins, outs;
    for (Node& n : g.Nodes) {
        CustomNodeDef* def = Find(n.Type);
        if (!def) continue;
        Interface(*def, ins, outs);
        RebuildPins(g, n, ins, PinKind::Input, "");
        RebuildPins(g, n, outs, PinKind::Output, "");
    }
    g.Dirty = true;
}

} // namespace CustomNodes
