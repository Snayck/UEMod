#pragma once
#include "imgui.h"
#include "imgui_node_editor.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
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
    Struct,   // in-place struct
    Array,    // TArray
    Any,      // any UEValue, runtime-typed
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
    bool        Selectable = false;
    bool        SelectorOpen = false;

    Pin(int id, const char* name, PinType type)
        : ID(id), Name(name), Type(type) {}
};

// session variable value; lives in Graph so it survives between runs
struct ScriptVar {
    PinType     Type = PinType::Any;
    std::string Str;
    double      Num = 0.0;
    bool        Bool = false;
    void*       Obj = nullptr;
};

struct Node {
    ed::NodeId          ID;
    std::string         Type;   // logic key ("ForEach", "GetVar", custom node name)
    std::string         Name;   // display label
    std::vector<Pin>    Inputs;
    std::vector<Pin>    Outputs;
    ImColor             Color;
    std::string         Meta;   // extra data (variable name for Get/SetVar)
    ImVec2              Pos = ImVec2(0, 0);
    int                 HookHandle = 0;   // live Pre/PostHook registration (>0 = on)

    // error feedback: written from executor threads, read by the renderer
    char                Error[96] = {};
    std::atomic<int64_t> ErrorNs{ 0 };
    std::atomic<int64_t> LastLogNs{ 0 };   // console-log throttle

    Node(int id, const char* type, ImColor color = ImColor(255, 255, 255))
        : ID(id), Type(type), Name(type), Color(color) {}
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
    bool                Dirty = false;
    std::atomic<bool>   Playing{ true };   // gates hook callbacks + frame ticks

    mutable std::mutex                VarsMutex;
    std::map<std::string, ScriptVar>  Variables;

    Graph() = default;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    // move: nodes live in a list (stable, never element-moved) so atomics are safe
    Graph(Graph&& o) noexcept
        : Nodes(std::move(o.Nodes)), Links(std::move(o.Links)),
          NextId(o.NextId), Dirty(o.Dirty), Playing(o.Playing.load()),
          Variables(std::move(o.Variables)) {}
    Graph& operator=(Graph&& o) noexcept {
        if (this != &o) {
            Nodes = std::move(o.Nodes);
            Links = std::move(o.Links);
            NextId = o.NextId;
            Dirty = o.Dirty;
            Playing.store(o.Playing.load());
            std::lock_guard<std::mutex> lk1(VarsMutex), lk2(o.VarsMutex);
            Variables = std::move(o.Variables);
        }
        return *this;
    }

    int GetNextId() { return NextId++; }

    Node* FindNodeByType(const char* type) {
        for (Node& n : Nodes) if (n.Type == type) return &n;
        return nullptr;
    }
};

	void RenderNodes(Graph& g);
	void PollInput(Graph& g);
	void RemoveLinksTouchingNode(Graph& g, const Node& node);

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
			case PinType::Any:    return ImColor(200, 200, 200);
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
