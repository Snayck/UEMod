#pragma once
#include "imgui.h"
#include "imgui_node_editor.h"
#include <string>
#include <vector>
#include <list>

#include "widgets.h"
#include "drawing.h"

namespace ed = ax::NodeEditor;
namespace Editor {

enum class PinType {
    Flow,     // exec.
    Bool,
    Int,
    Float,
    String,
    Name,     // FName
    Object,   // UObject*
    Struct,   // in-place struct]
    Array,    // TArray
    // Set, Map
};

enum class PinKind { Input, Output };

struct Node;

struct Pin {
    ed::PinId   ID;
    Node*		Node = nullptr;
    std::string Name;
    PinType		Type;
    PinKind		Kind = PinKind::Input;
	std::string DefaultValue;

    Pin(int id, const char* name, PinType type)
        : ID(id), Name(name), Type(type) {}
};

struct Node {
    ed::NodeId          ID;
    std::string         Name;
    std::vector<Pin>    Inputs;
    std::vector<Pin>    Outputs;
    ImColor             Color;
    // std::unique_ptr<NodeLogic> Logic;

    Node(int id, const char* name, ImColor color = ImColor(255, 255, 255))
        : ID(id), Name(name), Color(color) {}
};

struct Link {
    ed::LinkId ID;
    ed::PinId  StartPinID;
    ed::PinId  EndPinID;

    Link(int id, ed::PinId start, ed::PinId end)
        : ID(id), StartPinID(start), EndPinID(end) {}
};

struct Graph {
    std::list<Node>     Nodes;
    std::vector<Link>   Links;
    int                 NextId = 1;

    int GetNextId() { return NextId++; }
};

	void RenderNodes(Graph& g);
	void PollInput(Graph& g);

	inline ImColor PinColor(PinType t) {
		switch (t) {
			case PinType::Flow:   return ImColor(255, 255, 255);
			case PinType::Bool:   return ImColor(220,  48,  48);
			case PinType::Int:    return ImColor( 68, 201, 156);
			case PinType::Float:  return ImColor(147, 226,  74);
			case PinType::String: return ImColor(218,   0, 183);
			case PinType::Name:   return ImColor(200, 200,  90);
			case PinType::Object: return ImColor( 51, 150, 215);
			case PinType::Struct: return ImColor(180, 120, 255);
			case PinType::Array:  return ImColor(120, 120, 255);
			default:              return ImColor(255, 255, 255);
		}
	}
	inline ax::Drawing::IconType PinShape(PinType t) {
		return (t == PinType::Flow) ? ax::Drawing::IconType::Flow
									: ax::Drawing::IconType::Circle;
	}

	inline bool IsPinConnected(Graph& g, ed::PinId id) {
		for (Link& l : g.Links)
			if (l.StartPinID == id || l.EndPinID == id) return true;
		return false;
	}

	inline void DrawPinIcon(Graph& g, const Pin& pin) {
		const float sz = 20.0f;
		bool connected = IsPinConnected(g, pin.ID);
		ImColor c = PinColor(pin.Type);
		ax::Widgets::Icon(ImVec2(sz, sz), PinShape(pin.Type), connected, c, ImColor(32, 32, 32, 255));
	}
}