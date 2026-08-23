#pragma once
#include "graph.h"
#include <string>
#include <vector>

namespace Editor {

struct CustomNodeDef {
    std::string         Name;      // also the instance node Type
    std::string         Category;
    Graph               Body;      // contains one CustomInput + one CustomOutput node
    ed::EditorContext*  EdCtx = nullptr;
};

} // namespace Editor

struct CustomPinDesc {
    std::string     Name;
    Editor::PinType Type;
};

namespace CustomNodes {
    std::vector<Editor::CustomNodeDef>& All();
    Editor::CustomNodeDef* Find(const std::string& name);

    // create a def with fresh Input/Output interface nodes; returns null on dup name
    Editor::CustomNodeDef* Create(const std::string& name, const std::string& category);
    void Delete(const std::string& name);

    // interface of a def = the pins on its body's CustomInput/CustomOutput nodes
    void Interface(const Editor::CustomNodeDef& def,
                   std::vector<CustomPinDesc>& inputs,
                   std::vector<CustomPinDesc>& outputs);

    // spawn an instance of `def` into `g`
    Editor::Node* SpawnInstance(Editor::Graph& g, const Editor::CustomNodeDef& def);

    // rebuild instance nodes' pins in `g` to match (renamed) defs; keeps links
    // whose pin name+type survived
    void SyncInstances(Editor::Graph& g);

    // UI state shared between panels
    extern bool ShowCreate;
    extern std::string EditName;
}
