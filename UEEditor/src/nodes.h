#pragma once
#include "imgui.h"
#include "imgui_node_editor.h"
#include "graph.h"
#include <string>
#include <vector>

#ifdef GetClassName
#  undef GetClassName
#endif
#ifdef DrawText
#  undef DrawText
#endif

using namespace Editor;

namespace Nodes {

    namespace Col {
        const ImColor Event  = ImColor(255, 128, 128);
        const ImColor Flow   = ImColor(160, 160, 160);
        const ImColor Object = ImColor( 51, 150, 215);
        const ImColor Math   = ImColor(147, 226,  74);
        const ImColor Draw   = ImColor(230, 180,  80);
        const ImColor Debug  = ImColor(180, 120, 255);
    }

    inline void BuildNode(Node* node) {
        for (Pin& p : node->Inputs)  { p.Node = node; p.Kind = PinKind::Input; }
        for (Pin& p : node->Outputs) { p.Node = node; p.Kind = PinKind::Output; }
    }

    inline Node* Begin(Graph& g, const char* type, ImColor color, const char* displayName = nullptr) {
        g.Nodes.emplace_back(g.GetNextId(), type, color);
        g.Nodes.back().Name = displayName ? displayName : type;
        return &g.Nodes.back();
    }
    inline Pin& In(Graph& g, Node* n, const char* name, PinType t) {
        n->Inputs.emplace_back(g.GetNextId(), name, t); return n->Inputs.back();
    }
    inline Pin& Out(Graph& g, Node* n, const char* name, PinType t) {
        n->Outputs.emplace_back(g.GetNextId(), name, t); return n->Outputs.back();
    }

    // Events

    inline Node* SpawnScriptStartNode(Graph& g) {
        Node* n = Begin(g, "ScriptStart", Col::Event);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* OnKeyPress(Graph& g) {
        Node* n = Begin(g, "OnKeyPress", Col::Event);
        Out(g, n, "", PinType::Flow);
        Out(g, n, "Key", PinType::String);
        BuildNode(n); return n;
    }

    inline Node* OnFrameRender(Graph& g) {
        Node* n = Begin(g, "OnFrameRender", Col::Event);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* PreHook(Graph& g) {
        Node* n = Begin(g, "PreHook", Col::Event);
        In (g, n, "Function", PinType::String);
        In (g, n, "Block Original", PinType::Bool);
        Out(g, n, "", PinType::Flow);
        Out(g, n, "Caller", PinType::Object);
        Out(g, n, "Params", PinType::Any);
        BuildNode(n); return n;
    }

    inline Node* PostHook(Graph& g) {
        Node* n = Begin(g, "PostHook", Col::Event);
        In (g, n, "Function", PinType::String);
        Out(g, n, "", PinType::Flow);
        Out(g, n, "Caller", PinType::Object);
        Out(g, n, "Params", PinType::Any);
        Out(g, n, "Return", PinType::Any);
        BuildNode(n); return n;
    }

    inline Node* GetParam(Graph& g) {
        Node* n = Begin(g, "GetParam", Col::Event);
        In (g, n, "Params", PinType::Any);
        In (g, n, "Name", PinType::String);
        Out(g, n, "Value", PinType::Any);
        BuildNode(n); return n;
    }

    // Flow

    inline Node* Sequence(Graph& g) {
        Node* n = Begin(g, "Sequence", Col::Flow);
        In (g, n, "", PinType::Flow);
        Out(g, n, "Then 0", PinType::Flow);
        Out(g, n, "Then 1", PinType::Flow);
        Out(g, n, "Then 2", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* Branch(Graph& g) {
        Node* n = Begin(g, "Branch", Col::Flow);
        In (g, n, "", PinType::Flow);
        In (g, n, "Condition", PinType::Bool);
        Out(g, n, "True", PinType::Flow);
        Out(g, n, "False", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* ForEach(Graph& g) {
        Node* n = Begin(g, "ForEach", Col::Flow);
        In (g, n, "", PinType::Flow);
        In (g, n, "Array", PinType::Array);
        Out(g, n, "Loop Body", PinType::Flow);
        Out(g, n, "Element", PinType::Any);
        Out(g, n, "Index", PinType::Int);
        Out(g, n, "Completed", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* ForLoop(Graph& g) {
        Node* n = Begin(g, "ForLoop", Col::Flow);
        In (g, n, "", PinType::Flow);
        In (g, n, "First", PinType::Int);
        In (g, n, "Last", PinType::Int);
        Out(g, n, "Loop Body", PinType::Flow);
        Out(g, n, "Index", PinType::Int);
        Out(g, n, "Completed", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* WhileLoop(Graph& g) {
        Node* n = Begin(g, "WhileLoop", Col::Flow);
        In (g, n, "", PinType::Flow);
        In (g, n, "Condition", PinType::Bool);
        Out(g, n, "Loop Body", PinType::Flow);
        Out(g, n, "Completed", PinType::Flow);
        BuildNode(n); return n;
    }

    // Objects

    inline Node* GetWorld(Graph& g) {
        Node* n = Begin(g, "GetWorld", Col::Object);
        Out(g, n, "World", PinType::Object);
        BuildNode(n); return n;
    }

    inline Node* FindObject(Graph& g) {
        Node* n = Begin(g, "FindObject", Col::Object);
        In (g, n, "Name", PinType::String);
        In (g, n, "Class", PinType::String);
        Out(g, n, "Object", PinType::Object);
        BuildNode(n); return n;
    }

    inline Node* GetObjectsOfClass(Graph& g) {
        Node* n = Begin(g, "GetObjectsOfClass", Col::Object);
        In (g, n, "Class name", PinType::String);
        Out(g, n, "Objects", PinType::Array);
        BuildNode(n); return n;
    }

    inline Node* GetValue(Graph& g) {
        Node* n = Begin(g, "GetValue", Col::Object);
        In (g, n, "Target", PinType::Object);
        In (g, n, "Field", PinType::String);
        Out(g, n, "Value", PinType::Any);
        BuildNode(n); return n;
    }

    inline Node* SetValue(Graph& g) {
        Node* n = Begin(g, "SetValue", Col::Object);
        In (g, n, "", PinType::Flow);
        In (g, n, "Target", PinType::Object);
        In (g, n, "Field", PinType::String);
        In (g, n, "Value", PinType::Any);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* CallFunction(Graph& g) {
        Node* n = Begin(g, "CallFunction", Col::Object);
        In (g, n, "", PinType::Flow);
        In (g, n, "Target", PinType::Object);
        In (g, n, "Function", PinType::String);
        In (g, n, "Arg 0", PinType::Any);
        In (g, n, "Arg 1", PinType::Any);
        In (g, n, "Arg 2", PinType::Any);
        In (g, n, "Arg 3", PinType::Any);
        Out(g, n, "", PinType::Flow);
        Out(g, n, "Return", PinType::Any);
        BuildNode(n); return n;
    }

    inline Node* IsValid(Graph& g) {
        Node* n = Begin(g, "IsValid", Col::Object);
        In (g, n, "Object", PinType::Object);
        Out(g, n, "Valid", PinType::Bool);
        BuildNode(n); return n;
    }

    inline Node* IsA(Graph& g) {
        Node* n = Begin(g, "IsA", Col::Object);
        In (g, n, "Object", PinType::Object);
        In (g, n, "Class", PinType::String);
        Out(g, n, "Result", PinType::Bool);
        BuildNode(n); return n;
    }

    inline Node* GetName(Graph& g) {
        Node* n = Begin(g, "GetName", Col::Object);
        In (g, n, "Object", PinType::Object);
        Out(g, n, "Name", PinType::String);
        BuildNode(n); return n;
    }

    inline Node* GetClassName(Graph& g) {
        Node* n = Begin(g, "GetClassName", Col::Object);
        In (g, n, "Object", PinType::Object);
        Out(g, n, "Class", PinType::String);
        BuildNode(n); return n;
    }

    inline Node* GetFullName(Graph& g) {
        Node* n = Begin(g, "GetFullName", Col::Object);
        In (g, n, "Object", PinType::Object);
        Out(g, n, "Full Name", PinType::String);
        BuildNode(n); return n;
    }

    inline Node* GetAllFields(Graph& g) {
        Node* n = Begin(g, "GetAllFields", Col::Object);
        In (g, n, "Object", PinType::Object);
        Out(g, n, "Fields", PinType::Array);
        BuildNode(n); return n;
    }

    inline Node* GetAllMethods(Graph& g) {
        Node* n = Begin(g, "GetAllMethods", Col::Object);
        In (g, n, "Object", PinType::Object);
        Out(g, n, "Methods", PinType::Array);
        BuildNode(n); return n;
    }

    // Math

    inline Node* MakeBinaryMath(Graph& g, const char* name, PinType in, PinType out) {
        Node* n = Begin(g, name, Col::Math);
        In (g, n, "A", in);
        In (g, n, "B", in);
        Out(g, n, "Result", out);
        BuildNode(n); return n;
    }
    inline Node* Add(Graph& g)      { return MakeBinaryMath(g, "Add",      PinType::Float, PinType::Float); }
    inline Node* Subtract(Graph& g) { return MakeBinaryMath(g, "Subtract", PinType::Float, PinType::Float); }
    inline Node* Multiply(Graph& g) { return MakeBinaryMath(g, "Multiply", PinType::Float, PinType::Float); }
    inline Node* Divide(Graph& g)   { return MakeBinaryMath(g, "Divide",   PinType::Float, PinType::Float); }
    inline Node* Greater(Graph& g)  { return MakeBinaryMath(g, "A > B",    PinType::Float, PinType::Bool);  }
    inline Node* Less(Graph& g)     { return MakeBinaryMath(g, "A < B",    PinType::Float, PinType::Bool);  }
    inline Node* Equal(Graph& g)    { return MakeBinaryMath(g, "A == B",   PinType::Any,   PinType::Bool);  }
    inline Node* And(Graph& g)      { return MakeBinaryMath(g, "And",      PinType::Bool,  PinType::Bool);  }
    inline Node* Or(Graph& g)       { return MakeBinaryMath(g, "Or",       PinType::Bool,  PinType::Bool);  }

    inline Node* Not(Graph& g) {
        Node* n = Begin(g, "Not", Col::Math);
        In (g, n, "A", PinType::Bool);
        Out(g, n, "Result", PinType::Bool);
        BuildNode(n); return n;
    }

    // Draw

    inline Node* DrawText(Graph& g) {
        Node* n = Begin(g, "DrawText", Col::Draw);
        In (g, n, "", PinType::Flow);
        In (g, n, "Text", PinType::Any);
        In (g, n, "X", PinType::Float);
        In (g, n, "Y", PinType::Float);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    inline Node* DrawLine(Graph& g) {
        Node* n = Begin(g, "DrawLine", Col::Draw);
        In (g, n, "", PinType::Flow);
        In (g, n, "X1", PinType::Float);
        In (g, n, "Y1", PinType::Float);
        In (g, n, "X2", PinType::Float);
        In (g, n, "Y2", PinType::Float);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    // Debug

    inline Node* Print(Graph& g) {
        Node* n = Begin(g, "Print", Col::Debug);
        In (g, n, "", PinType::Flow);
        In (g, n, "Message", PinType::Any);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    // Values

    inline Node* Literal(Graph& g) {
        Node* n = Begin(g, "Literal", Col::Math);
        Out(g, n, "Value", PinType::Float).Selectable = true;
        BuildNode(n); return n;
    }

    inline Node* ArrayLength(Graph& g) {
        Node* n = Begin(g, "ArrayLength", Col::Object);
        In (g, n, "Array", PinType::Array);
        Out(g, n, "Length", PinType::Int);
        BuildNode(n); return n;
    }

    inline Node* ArrayGet(Graph& g) {
        Node* n = Begin(g, "ArrayGet", Col::Object);
        In (g, n, "Array", PinType::Array);
        In (g, n, "Index", PinType::Int);
        Out(g, n, "Element", PinType::Any);
        BuildNode(n); return n;
    }

    // ---- variables ----

    inline Node* GetVar(Graph& g, const std::string& varName, PinType type) {
        Node* n = Begin(g, "GetVar", Col::Object, ("Get " + varName).c_str());
        n->Meta = varName;
        Out(g, n, varName.c_str(), type);
        BuildNode(n); return n;
    }
    inline Node* SetVar(Graph& g, const std::string& varName, PinType type) {
        Node* n = Begin(g, "SetVar", Col::Object, ("Set " + varName).c_str());
        n->Meta = varName;
        In (g, n, "", PinType::Flow);
        In (g, n, "Value", type);
        Out(g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }

    // ---- custom nodes ----

    // interface nodes inside a custom node's body
    inline Node* SpawnCustomIO(Graph& g, const char* ioType) {
        Node* n = Begin(g, ioType, Col::Event);
        if (strcmp(ioType, "CustomInput") == 0)
            Out(g, n, "", PinType::Flow);
        else
            In (g, n, "", PinType::Flow);
        BuildNode(n); return n;
    }
}

