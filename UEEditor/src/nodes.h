#pragma once
#include "imgui.h"
#include "imgui_node_editor.h"
#include "graph.h"
#include <string>
#include <vector>

using namespace Editor;

namespace Nodes {
    inline void BuildNode(Node* node) {
        for (Pin& p : node->Inputs)  { p.Node = node; p.Kind = PinKind::Input; }
        for (Pin& p : node->Outputs) { p.Node = node; p.Kind = PinKind::Output; }
    }

    inline Node* SpawnInitScriptNode(Graph& g) {
        g.Nodes.emplace_back(g.GetNextId(), "OnInitialize", ImColor(255, 128, 128));
        Node* node = &g.Nodes.back();
        node->Outputs.emplace_back(g.GetNextId(), "", PinType::Flow);
        BuildNode(node);
        return node;
    }

    inline Node* GetObjectsOfClass(Graph& g) {
        g.Nodes.emplace_back(g.GetNextId(), "GetObjectsOfClass", ImColor(255, 128, 128));
        Node* node = &g.Nodes.back();

        node->Inputs.emplace_back(g.GetNextId(), "Class name", PinType::String);
        node->Outputs.emplace_back(g.GetNextId(), "Objects", PinType::Array);

        BuildNode(node);
        return node;
    }

    inline Node* ForEach(Graph& g) {
        g.Nodes.emplace_back(g.GetNextId(), "ForEach", ImColor(255, 128, 128));
        Node* node = &g.Nodes.back();
        
        node->Inputs.emplace_back(g.GetNextId(), "", PinType::Flow);
        node->Inputs.emplace_back(g.GetNextId(), "Array", PinType::Array);

        node->Outputs.emplace_back(g.GetNextId(), "Callback", PinType::Flow);
        node->Outputs.emplace_back(g.GetNextId(), "Element", PinType::Object);
        node->Outputs.emplace_back(g.GetNextId(), "Loop index", PinType::Int);
        node->Outputs.emplace_back(g.GetNextId(), "", PinType::Flow);

        BuildNode(node);
        return node;
    }

    inline Node* SetField(Graph& g) {
        g.Nodes.emplace_back(g.GetNextId(), "SetField", ImColor(255, 128, 128));
        Node* node = &g.Nodes.back();
        
        node->Inputs.emplace_back(g.GetNextId(), "", PinType::Flow);
        node->Inputs.emplace_back(g.GetNextId(), "Target", PinType::Object);
        node->Inputs.emplace_back(g.GetNextId(), "Field", PinType::String);
        node->Inputs.emplace_back(g.GetNextId(), "Value", PinType::String);

        node->Outputs.emplace_back(g.GetNextId(), "", PinType::Flow);
        
        BuildNode(node);
        return node;
    }
}
