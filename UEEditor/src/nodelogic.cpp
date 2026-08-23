#include "nodelogic.h"

#ifndef UEEDITOR_WITH_BACKEND

// -------- standalone .exe: no backend --------
namespace Exec {
    void InitBackend() {}
    void WaitForInit() {}
    void ShutdownBackend() {}
    bool BackendReady() { return false; }
    const char* BackendStatus() { return "standalone (no backend)"; }
    void RunScriptNode(Editor::Graph&, Editor::Node&) {}
    void FireKeyPress(Editor::Graph&, const char*) {}
    bool StartHook(Editor::Graph&, Editor::Node&) { return false; }
    void StopHook(Editor::Node&) {}
    void TickFrame(Editor::Graph&) {}
    void* ProbeInputClass(Editor::Graph&, Editor::Node&, const char*) { return nullptr; }
    bool ApplyStringToField(void*, const std::string&, const std::string&) { return false; }
}

#else
// -------- injected build --------
#include "imgui.h"
#include "UEHook.h"
#include "customnodes.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

using Editor::Graph;
using Editor::Node;
using Editor::Pin;
using Editor::Link;
using Editor::PinType;
using Editor::PinKind;
using Editor::CustomNodeDef;
using Editor::ScriptVar;

namespace {

// boxed UEValue, or a list of them (array outputs that aren't a live TArray)
struct EValue {
    UEValue v;
    std::vector<UEValue> list;
    bool isList = false;
    EValue() {}
    EValue(UEValue x) : v(std::move(x)) {}
    static EValue List(std::vector<UEValue> l) { EValue e; e.list = std::move(l); e.isList = true; return e; }
};

// ---- pin/node lookup helpers -------------------------------------------------
Pin* FindPinById(Graph& g, ed::PinId id) {
    if (!id) return nullptr;
    for (Node& n : g.Nodes) {
        for (Pin& p : n.Inputs)  if (p.ID == id) return &p;
        for (Pin& p : n.Outputs) if (p.ID == id) return &p;
    }
    return nullptr;
}
Pin* InPin(Node& n, const char* name) {
    for (Pin& p : n.Inputs) if (p.Name == name) return &p;
    return nullptr;
}
Pin* OutPin(Node& n, const char* name) {
    for (Pin& p : n.Outputs) if (p.Name == name) return &p;
    return nullptr;
}
Pin* FirstFlowOut(Node& n) {
    for (Pin& p : n.Outputs) if (p.Type == PinType::Flow) return &p;
    return nullptr;
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// red flash on the node + console line, throttled to 1/s
void FailNode(Node& n, const std::string& msg) {
    snprintf(n.Error, sizeof n.Error, "%s", msg.c_str());
    int64_t now = NowNs();
    n.ErrorNs.store(now, std::memory_order_relaxed);
    if (now - n.LastLogNs.load(std::memory_order_relaxed) >= 1000000000LL) {
        n.LastLogNs.store(now, std::memory_order_relaxed);
        printf("[node error] %s: %s\n", n.Name.c_str(), n.Error);
    }
}

const char* KindName(UEValueKind k) {
    switch (k) {
    case UEValueKind::Bool:   return "Bool";
    case UEValueKind::Int:    return "Int";
    case UEValueKind::Float:  return "Float";
    case UEValueKind::String: return "String";
    case UEValueKind::Name:   return "Name";
    case UEValueKind::Object: return "Object";
    case UEValueKind::Struct: return "Struct";
    case UEValueKind::Array:  return "Array";
    case UEValueKind::Set:    return "Set";
    case UEValueKind::Map:    return "Map";
    default:                  return "empty";
    }
}

struct Ctx;
struct NodeLogic {
    virtual EValue Evaluate(Ctx&, Node&, Pin&) { return {}; }
    virtual void   Execute(Ctx&, Node&);   // default: run first flow output
    // outputs only exist after Execute() ran (loops, calls)
    virtual bool   ImpureOutputs() const { return false; }
    virtual ~NodeLogic() {}
};
NodeLogic* LogicFor(const std::string& name);

struct Ctx {
    Graph& g;
    std::unordered_map<const void*, EValue>  outCache;   // impure output pin -> value
    std::unordered_map<const void*, UEParams> paramsCache; // hook Params pin -> frame
    Graph* varsG = nullptr;             // variable store (outer script when nested)
    std::vector<EValue>* returnSlot = nullptr; // custom node body -> instance results
    bool returned = false;
    explicit Ctx(Graph& gg, Graph* varSource = nullptr)
        : g(gg), varsG(varSource ? varSource : &gg) {}

    Pin* SourcePin(Pin& in) {
        for (Link& l : g.Links) if (l.EndPinID == in.ID) return FindPinById(g, l.StartPinID);
        return nullptr;
    }

    EValue Literal(Pin& in) {
        const std::string& s = in.DefaultValue;
        switch (in.Type) {
        case PinType::Float: return EValue(UEValue::MakeFloat(atof(s.c_str())));
        case PinType::Int:   return EValue(UEValue::MakeInt(atoll(s.c_str())));
        case PinType::Bool:  return EValue(UEValue::MakeBool(s == "true" || s == "1" || s == "True"));
        default:             return EValue(UEValue::MakeString(s));
        }
    }

    EValue Pull(Pin* in) {
        if (!in) return {};
        if (Pin* src = SourcePin(*in)) {
            auto it = outCache.find(src);
            if (it != outCache.end()) return it->second;
            if (src->Node) {
                NodeLogic* logic = LogicFor(src->Node->Type);
                if (logic->ImpureOutputs()) {
                    Fail(*src->Node, "'" + src->Name + "' read before this node executed. " +
                        "Its exec input must be wired into the chain " +
                        "(for nested loops: outer 'Loop Body' -> this node's exec input).");
                    return {};
                }
                return logic->Evaluate(*this, *src->Node, *src);
            }
        }
        return Literal(*in);
    }
    EValue Pull(Node& n, const char* name) { return Pull(InPin(n, name)); }

    void RunFrom(Pin& execOut) {
        for (Link& l : g.Links) if (l.StartPinID == execOut.ID) {
            if (Pin* dst = FindPinById(g, l.EndPinID))
                if (dst->Node) LogicFor(dst->Node->Type)->Execute(*this, *dst->Node);
        }
    }
    void RunNamed(Node& n, const char* outName) { if (Pin* p = OutPin(n, outName)) RunFrom(*p); }
    void RunNext(Node& n) { if (Pin* p = FirstFlowOut(n)) RunFrom(*p); }
    void SetOut(Node& n, const char* name, const EValue& val) { if (Pin* p = OutPin(n, name)) outCache[p] = val; }
    void Fail(Node& n, const std::string& msg) { FailNode(n, msg); }
};

void NodeLogic::Execute(Ctx& ctx, Node& n) { ctx.RunNext(n); }

std::vector<UEValue> ToList(const EValue& e) {
    if (e.isList) return e.list;
    std::vector<UEValue> out;
    if (e.v.Kind == UEValueKind::Array) {
        UEArrayRef arr(e.v);
        for (int i = 0; i < arr.Num(); ++i) out.push_back(arr.At(i));
    }
    return out;
}

void* RequireObject(Ctx& c, Node& n, const char* pinName) {
    Pin* p = InPin(n, pinName);
    if (!p) { c.Fail(n, std::string(pinName) + ": pin missing"); return nullptr; }
    if (!c.SourcePin(*p) && p->DefaultValue.empty()) {
        c.Fail(n, std::string(pinName) + ": not connected");
        return nullptr;
    }
    EValue e = c.Pull(p);
    if (e.v.Kind == UEValueKind::Object) {
        if (e.v.Ptr) return e.v.Ptr;
        c.Fail(n, std::string(pinName) + ": object is null");
        return nullptr;
    }
    c.Fail(n, std::string(pinName) + ": expected Object, got " + KindName(e.v.Kind));
    return nullptr;
}

// ---- data nodes (Evaluate) ---------------------------------------------------
struct GetWorldLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject w = UE::GetWorld();
        if (!w) { c.Fail(n, "GWorld is null (detection failed or no world loaded yet)"); return {}; }
        return EValue(UEValue::MakeObject(w.GetAddress()));
    }
};
struct FindObjectLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        std::string name = c.Pull(n, "Name").v.AsString();
        UEObject o = UE::FindObject(name, c.Pull(n, "Class").v.AsString());
        if (!o) c.Fail(n, "no object named '" + name + "' found");
        return EValue(UEValue::MakeObject(o.GetAddress()));
    }
};
struct GetObjectsOfClassLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        std::string cls = c.Pull(n, "Class name").v.AsString();
        std::vector<UEValue> out;
        for (UEObject& o : UE::GetObjectsOfClass(cls))
            out.push_back(UEValue::MakeObject(o.GetAddress()));
        if (out.empty()) c.Fail(n, "no objects of class '" + cls + "' found");
        return EValue::List(out);
    }
};
struct GetValueLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Target"));
        if (!t) return {};
        std::string field = c.Pull(n, "Field").v.AsString();
        UEValue v = t.GetValue(field);
        if (v.IsNone()) { c.Fail(n, "field '" + field + "' not found on " + t.GetClassName()); return {}; }
        return EValue(v);
    }
};
struct GetNameLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Object"));
        return t ? EValue(UEValue::MakeString(t.GetName())) : EValue{};
    }
};
struct GetClassNameLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Object"));
        return t ? EValue(UEValue::MakeString(t.GetClassName())) : EValue{};
    }
};
struct GetFullNameLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Object"));
        return t ? EValue(UEValue::MakeString(t.GetFullName())) : EValue{};
    }
};
struct IsValidLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(c.Pull(n, "Object").v.AsObject());
        return EValue(UEValue::MakeBool(t && t.IsValid()));
    }
};
struct IsALogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Object"));
        return EValue(UEValue::MakeBool(t && t.IsA(c.Pull(n, "Class").v.AsString())));
    }
};
struct GetAllFieldsLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Object"));
        std::vector<UEValue> out;
        if (t) for (UEProperty& p : t.GetClass().GetAllProperties()) out.push_back(UEValue::MakeString(p.GetName()));
        return EValue::List(out);
    }
};
struct GetAllMethodsLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        UEObject t(RequireObject(c, n, "Object"));
        std::vector<UEValue> out;
        if (t) for (UEFunction& f : t.GetClass().GetAllFunctions()) out.push_back(UEValue::MakeString(f.GetName()));
        return EValue::List(out);
    }
};
struct ArrayLengthLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        return EValue(UEValue::MakeInt((int64_t)ToList(c.Pull(n, "Array")).size()));
    }
};
struct ArrayGetLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        auto items = ToList(c.Pull(n, "Array"));
        int64_t i = c.Pull(n, "Index").v.AsInt();
        if (i >= 0 && i < (int64_t)items.size()) return EValue(items[i]);
        return {};
    }
};
struct LiteralLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin& out) override { return c.Literal(out); }
};

// ---- variables ----

static EValue VarToValue(const ScriptVar& v) {
    switch (v.Type) {
    case PinType::Bool:   return EValue(UEValue::MakeBool(v.Bool));
    case PinType::Int:    return EValue(UEValue::MakeInt((int64_t)v.Num));
    case PinType::Float:  return EValue(UEValue::MakeFloat(v.Num));
    case PinType::Name:   return EValue(UEValue::MakeName(v.Str));
    case PinType::Object: return EValue(UEValue::MakeObject(v.Obj));
    case PinType::String: return EValue(UEValue::MakeString(v.Str));
    default: // Any: whatever was last written
        if (v.Obj)          return EValue(UEValue::MakeObject(v.Obj));
        if (!v.Str.empty()) return EValue(UEValue::MakeString(v.Str));
        return EValue(UEValue::MakeFloat(v.Num));
    }
}

struct GetVarLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        ScriptVar v;
        {
            std::lock_guard<std::mutex> lock(c.varsG->VarsMutex);
            auto it = c.varsG->Variables.find(n.Meta);
            if (it == c.varsG->Variables.end()) {
                c.Fail(n, "no variable '" + n.Meta + "'");
                return {};
            }
            v = it->second;
        }
        return VarToValue(v);
    }
};
struct SetVarLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        EValue e = c.Pull(n, "Value");
        std::lock_guard<std::mutex> lock(c.varsG->VarsMutex);
        ScriptVar& v = c.varsG->Variables[n.Meta];
        switch (v.Type) {
        case PinType::Bool:   v.Bool = e.v.AsBool(); break;
        case PinType::Int:    v.Num = (double)e.v.AsInt(); break;
        case PinType::Float:  v.Num = e.v.AsFloat(); break;
        case PinType::Name:
        case PinType::String: v.Str = e.v.AsString(); break;
        case PinType::Object: v.Obj = e.v.AsObject(); break;
        default: // Any
            v.Obj = (e.v.Kind == UEValueKind::Object) ? e.v.Ptr : nullptr;
            v.Str = (e.v.Kind == UEValueKind::String || e.v.Kind == UEValueKind::Name) ? e.v.Str : "";
            v.Num = (e.v.Kind == UEValueKind::Int) ? (double)e.v.Int
                  : (e.v.Kind == UEValueKind::Float) ? e.v.Float : 0.0;
            break;
        }
        c.RunNext(n);
    }
};

// math family, dispatched on node name
struct BinaryMathLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        const std::string& op = n.Name;
        UEValue a = c.Pull(n, "A").v, b = c.Pull(n, "B").v;
        if (op == "Add")      return EValue(UEValue::MakeFloat(a.AsFloat() + b.AsFloat()));
        if (op == "Subtract") return EValue(UEValue::MakeFloat(a.AsFloat() - b.AsFloat()));
        if (op == "Multiply") return EValue(UEValue::MakeFloat(a.AsFloat() * b.AsFloat()));
        if (op == "Divide")   return EValue(UEValue::MakeFloat(b.AsFloat() != 0 ? a.AsFloat() / b.AsFloat() : 0.0));
        if (op == "A > B")    return EValue(UEValue::MakeBool(a.AsFloat() >  b.AsFloat()));
        if (op == "A < B")    return EValue(UEValue::MakeBool(a.AsFloat() <  b.AsFloat()));
        if (op == "And")      return EValue(UEValue::MakeBool(a.AsBool() && b.AsBool()));
        if (op == "Or")       return EValue(UEValue::MakeBool(a.AsBool() || b.AsBool()));
        if (op == "A == B") {
            bool eq = (a.Kind == UEValueKind::String || b.Kind == UEValueKind::String)
                        ? a.AsString() == b.AsString()
                        : a.AsFloat() == b.AsFloat();
            return EValue(UEValue::MakeBool(eq));
        }
        return {};
    }
};
struct NotLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override { return EValue(UEValue::MakeBool(!c.Pull(n, "A").v.AsBool())); }
};
struct GetParamLogic : NodeLogic {
    EValue Evaluate(Ctx& c, Node& n, Pin&) override {
        Pin* pin = InPin(n, "Params");
        Pin* src = pin ? c.SourcePin(*pin) : nullptr;
        auto it = src ? c.paramsCache.find(src) : c.paramsCache.end();
        if (it == c.paramsCache.end()) {
            c.Fail(n, "Params: not connected to a live hook's Params pin");
            return {};
        }
        std::string name = c.Pull(n, "Name").v.AsString();
        UEValue v = it->second.GetValue(name);
        if (v.IsNone()) c.Fail(n, "param '" + name + "' not found on this function");
        return EValue(v);
    }
};

// ---- custom node execution ----

struct CustomOutputLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        if (c.returnSlot)
            for (Pin& p : n.Inputs) {
                if (p.Type == PinType::Flow) continue;
                c.returnSlot->push_back(c.Pull(&p));
            }
        c.returned = true;   // terminal node
    }
};

struct CustomInstanceLogic : NodeLogic {
    bool ImpureOutputs() const override { return true; }
    void Execute(Ctx& c, Node& n) override {
        CustomNodeDef* def = CustomNodes::Find(n.Type);
        Node* inNode  = def ? def->Body.FindNodeByType("CustomInput")  : nullptr;
        Node* outNode = def ? def->Body.FindNodeByType("CustomOutput") : nullptr;
        if (!def || !inNode || !outNode) {
            c.Fail(n, "custom node definition missing or broken");
            c.RunNext(n);
            return;
        }
        static thread_local int depth = 0;
        if (depth >= 16) {
            c.Fail(n, "custom node recursion too deep");
            c.RunNext(n);
            return;
        }

        Ctx bc(def->Body, &c.g);   // body sees the outer script's variables
        std::vector<EValue> results;
        bc.returnSlot = &results;

        for (Pin& op : inNode->Outputs) {          // seed body inputs by name
            if (op.Type == PinType::Flow) continue;
            if (Pin* ip = InPin(n, op.Name.c_str()))
                bc.outCache[&op] = c.Pull(ip);
        }

        ++depth;
        bc.RunNext(*inNode);
        --depth;

        if (bc.returned) {
            size_t i = 0;
            for (Pin& ip : outNode->Inputs) {
                if (ip.Type == PinType::Flow) continue;
                if (i < results.size()) c.SetOut(n, ip.Name.c_str(), results[i]);
                ++i;
            }
        }
        c.RunNext(n);
    }
};

// ---- action / flow nodes (Execute) -------------------------------------------
struct SequenceLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        c.RunNamed(n, "Then 0"); c.RunNamed(n, "Then 1"); c.RunNamed(n, "Then 2");
    }
};
struct BranchLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override { c.RunNamed(n, c.Pull(n, "Condition").v.AsBool() ? "True" : "False"); }
};
struct ForEachLogic : NodeLogic {
    bool ImpureOutputs() const override { return true; }
    void Execute(Ctx& c, Node& n) override {
        Pin* ap = InPin(n, "Array");
        if (ap && !c.SourcePin(*ap) && ap->DefaultValue.empty())
            c.Fail(n, "Array: not connected");
        auto items = ToList(c.Pull(n, "Array"));
        for (size_t i = 0; i < items.size(); ++i) {
            c.SetOut(n, "Element", EValue(items[i]));
            c.SetOut(n, "Index", EValue(UEValue::MakeInt((int64_t)i)));
            c.RunNamed(n, "Loop Body");
        }
        c.RunNamed(n, "Completed");
    }
};
struct ForLoopLogic : NodeLogic {
    bool ImpureOutputs() const override { return true; }
    void Execute(Ctx& c, Node& n) override {
        int64_t first = c.Pull(n, "First").v.AsInt(), last = c.Pull(n, "Last").v.AsInt();
        for (int64_t i = first; i < last; ++i) {
            c.SetOut(n, "Index", EValue(UEValue::MakeInt(i)));
            c.RunNamed(n, "Loop Body");
        }
        c.RunNamed(n, "Completed");
    }
};
struct WhileLoopLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        int guard = 0;
        while (c.Pull(n, "Condition").v.AsBool() && guard++ < 100000) c.RunNamed(n, "Loop Body");
        c.RunNamed(n, "Completed");
    }
};
struct SetValueLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        UEObject t(RequireObject(c, n, "Target"));
        std::string field = c.Pull(n, "Field").v.AsString();
        UEValue val = c.Pull(n, "Value").v;
        if (!t) { c.RunNext(n); return; }
        UEProperty p = t.FindProp(field);
        if (!p) {
            c.Fail(n, "field '" + field + "' not found on " + t.GetClassName());
        } else {
            if (val.Kind == UEValueKind::String) {  // coerce typed text to the field's real type
                std::string tn = p.GetTypeName();
                if (tn == "FloatProperty" || tn == "DoubleProperty") val = UEValue::MakeFloat(atof(val.Str.c_str()));
                else if (tn == "BoolProperty")                        val = UEValue::MakeBool(val.Str == "true" || val.Str == "1");
                else if (tn.find("IntProperty") != std::string::npos || tn == "ByteProperty") val = UEValue::MakeInt(atoll(val.Str.c_str()));
            }
            if (!t.SetValue(field, val)) c.Fail(n, "write to '" + field + "' failed");
        }
        c.RunNext(n);
    }
};
struct CallFunctionLogic : NodeLogic {
    bool ImpureOutputs() const override { return true; }
    void Execute(Ctx& c, Node& n) override {
        UEObject t(RequireObject(c, n, "Target"));
        std::string fn = c.Pull(n, "Function").v.AsString();
        std::vector<UEValue> args;
        for (const char* an : { "Arg 0", "Arg 1", "Arg 2", "Arg 3" }) {
            Pin* ap = InPin(n, an);
            if (ap && (c.SourcePin(*ap) || !ap->DefaultValue.empty())) args.push_back(c.Pull(ap).v);
        }
        bool ok = false;
        UEValue r = t ? t.CallByName(fn, args, &ok) : UEValue{};
        if (t && !ok) c.Fail(n, "call '" + fn + "' failed (function not found?)");
        c.SetOut(n, "Return", EValue(r));
        c.RunNext(n);
    }
};
struct PrintLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        printf("[Print] %s\n", c.Pull(n, "Message").v.ToString().c_str());
        c.RunNext(n);
    }
};
struct DrawTextLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        ImVec2 pos((float)c.Pull(n, "X").v.AsFloat(), (float)c.Pull(n, "Y").v.AsFloat());
        ImGui::GetForegroundDrawList()->AddText(pos, IM_COL32_WHITE, c.Pull(n, "Text").v.ToString().c_str());
        c.RunNext(n);
    }
};
struct DrawLineLogic : NodeLogic {
    void Execute(Ctx& c, Node& n) override {
        ImVec2 a((float)c.Pull(n, "X1").v.AsFloat(), (float)c.Pull(n, "Y1").v.AsFloat());
        ImVec2 b((float)c.Pull(n, "X2").v.AsFloat(), (float)c.Pull(n, "Y2").v.AsFloat());
        ImGui::GetForegroundDrawList()->AddLine(a, b, IM_COL32_WHITE, 1.5f);
        c.RunNext(n);
    }
};

// ---- registry ---------------------------------------------------------------
NodeLogic* LogicFor(const std::string& name) {
    static NodeLogic            base;      // fallback: flow pass-through
    static GetWorldLogic        getWorld;
    static FindObjectLogic      findObject;
    static GetObjectsOfClassLogic getObjects;
    static GetValueLogic        getValue;
    static GetNameLogic         getName;
    static GetClassNameLogic    getClassName;
    static GetFullNameLogic     getFullName;
    static IsValidLogic         isValid;
    static IsALogic             isA;
    static GetAllFieldsLogic    getFields;
    static GetAllMethodsLogic   getMethods;
    static ArrayLengthLogic     arrLen;
    static ArrayGetLogic        arrGet;
    static LiteralLogic         literal;
    static BinaryMathLogic      math;
    static NotLogic             notLogic;
    static GetParamLogic        getParam;
    static SequenceLogic        sequence;
    static BranchLogic          branch;
    static ForEachLogic         forEach;
    static ForLoopLogic         forLoop;
    static WhileLoopLogic       whileLoop;
    static SetValueLogic        setValue;
    static CallFunctionLogic    callFn;
    static PrintLogic           print;
    static DrawTextLogic        drawText;
    static DrawLineLogic        drawLine;
    static GetVarLogic          getVar;
    static SetVarLogic          setVar;
    static CustomOutputLogic    customOut;
    static CustomInstanceLogic  customInstance;

    static const std::unordered_map<std::string, NodeLogic*> map = {
        { "GetWorld", &getWorld }, { "FindObject", &findObject }, { "GetObjectsOfClass", &getObjects },
        { "GetValue", &getValue }, { "GetName", &getName }, { "GetClassName", &getClassName },
        { "GetFullName", &getFullName }, { "IsValid", &isValid }, { "IsA", &isA },
        { "GetAllFields", &getFields }, { "GetAllMethods", &getMethods },
        { "ArrayLength", &arrLen }, { "ArrayGet", &arrGet }, { "Literal", &literal },
        { "Add", &math }, { "Subtract", &math }, { "Multiply", &math }, { "Divide", &math },
        { "A > B", &math }, { "A < B", &math }, { "A == B", &math }, { "And", &math }, { "Or", &math },
        { "Not", &notLogic }, { "GetParam", &getParam },
        { "GetVar", &getVar }, { "SetVar", &setVar }, { "CustomOutput", &customOut },
        { "Sequence", &sequence }, { "Branch", &branch }, { "ForEach", &forEach },
        { "ForLoop", &forLoop }, { "WhileLoop", &whileLoop },
        { "SetValue", &setValue }, { "CallFunction", &callFn }, { "Print", &print },
        { "DrawText", &drawText }, { "DrawLine", &drawLine },
    };
    auto it = map.find(name);
    if (it != map.end()) return it->second;
    if (CustomNodes::Find(name)) return &customInstance;
    return &base;
}

static int RegisterHook(Graph& g, Node& node, bool post) {
    Pin* fnPin = InPin(node, "Function");
    std::string fn = fnPin ? fnPin->DefaultValue : "";
    if (fn.empty()) {
        FailNode(node, "Function: not set");
        return 0;
    }
    Node* np = &node;
    Graph* gp = &g;
    std::atomic<bool>* gate = &g.Playing;
    auto cb = [gp, np, post, gate](Hooking::HookContext& hc) {
        if (!gate->load(std::memory_order_relaxed)) return;
        Ctx ctx(*gp);
        ctx.SetOut(*np, "Caller", EValue(UEValue::MakeObject(hc.Caller.GetAddress())));
        if (Pin* pp = OutPin(*np, "Params")) ctx.paramsCache[pp] = hc.Params;
        if (!post) {
            if (Pin* b = InPin(*np, "Block Original"))
                if (ctx.Pull(b).v.AsBool()) hc.ShouldCall = false;
        }
        ctx.RunNext(*np);
    };
    int h = post ? Hooking::AddPostByName(fn, cb)
                 : Hooking::AddPreByName(fn, cb);
    if (!h) FailNode(node, "function '" + fn + "' not found");
    return h;
}

} // namespace

namespace Exec {

namespace {
    std::atomic<int> g_InitState{ 0 };   // 0 = running, 1 = ok, -1 = failed
    std::thread g_InitThread;
}

void InitBackend() {
    if (g_InitThread.joinable()) return;
    g_InitThread = std::thread([] {
        UEHookConfig cfg;
        cfg.verbose = true;
        g_InitState.store(UE::Initialize(cfg) ? 1 : -1, std::memory_order_release);
    });
}

void WaitForInit() {
    if (g_InitThread.joinable())
        g_InitThread.join();
}

void ShutdownBackend() {
    UE::Shutdown();
}

bool BackendReady() { return g_InitState.load(std::memory_order_acquire) == 1; }

const char* BackendStatus()
{
    switch (g_InitState.load(std::memory_order_acquire)) {
    case 1:  return "ready";
    case -1: return "init failed";
    default: return "initializing...";
    }
}

bool StartHook(Graph& g, Node& n) {
    if (!UE::IsInitialized()) {
        FailNode(n, std::string("backend ") + BackendStatus());
        return false;
    }
    if (n.HookHandle) return true;   // already running
    int h = RegisterHook(g, n, n.Type == "PostHook");
    if (h) n.HookHandle = h;
    return h != 0;
}

void StopHook(Node& n) {
    if (n.HookHandle) {
        Hooking::Remove(n.HookHandle);
        n.HookHandle = 0;
    }
}

void RunScriptNode(Graph& g, Node& n) {
    if (!UE::IsInitialized()) {
        FailNode(n, std::string("backend ") + BackendStatus());
        return;
    }
    Node* np = &n;
    Graph* gp = &g;
    bool ran = UE::DispatchSync([&] {
        Ctx ctx(*gp);
        ctx.RunNext(*np);
    }, 2000);
    if (!ran)
        FailNode(n, "game-thread dispatch timed out (ProcessEvent pump not running?)");
}

void TickFrame(Graph& g) {
    if (!UE::IsInitialized()) return;
    Ctx ctx(g);   // frame-render + draw run on the UI thread (reads only; writes here are unsafe)
    for (Node& n : g.Nodes)
        if (n.Type == "OnFrameRender") ctx.RunNext(n);
}

void FireKeyPress(Graph& g, const char* key) {
    if (!UE::IsInitialized()) return;
    std::string k = key;
    UE::DispatchSync([&] {
        Ctx ctx(g);
        for (Node& n : g.Nodes)
            if (n.Type == "OnKeyPress") {
                ctx.SetOut(n, "Key", EValue(UEValue::MakeString(k)));
                ctx.RunNext(n);
            }
    }, 1000);
}

void* ProbeInputClass(Graph& g, Node& n, const char* pinName) {
    if (!UE::IsInitialized()) return nullptr;
    Ctx ctx(g);
    Pin* p = InPin(n, pinName);
    if (!p) return nullptr;
    EValue e = ctx.Pull(p);
    if (e.v.Kind != UEValueKind::Object || !e.v.Ptr) return nullptr;
    UEClass c = UEObject(e.v.Ptr).GetClass();
    return c ? c.GetAddress() : nullptr;
}

bool ApplyStringToField(void* obj, const std::string& path, const std::string& text) {
    UEObject root(obj);
    if (!root || path.empty()) return false;

    std::vector<std::string> segs;
    std::string cur;
    for (char c : path) {
        if (c == '.') { segs.push_back(cur); cur.clear(); }
        else cur += c;
    }
    segs.push_back(cur);
    if (segs.empty() || segs[0].empty()) return false;

    UEValue v;
    UEStructRef ref;
    UEProperty leaf = root.FindProp(segs[0]);
    if (!leaf) return false;
    if (segs.size() == 1) {
        UEValue val = UEValue::MakeString(text);
        std::string tn = leaf.GetTypeName();
        if (tn == "FloatProperty" || tn == "DoubleProperty") val = UEValue::MakeFloat(atof(text.c_str()));
        else if (tn == "BoolProperty")                        val = UEValue::MakeBool(text == "true" || text == "1");
        else if (tn.find("IntProperty") != std::string::npos || tn == "ByteProperty") val = UEValue::MakeInt(atoll(text.c_str()));
        else if (tn == "NameProperty")                        val = UEValue::MakeName(text);
        bool ok = false;
        UE::DispatchSync([&] { ok = root.SetValue(segs[0], val); });
        return ok;
    }

    v = root.GetValue(segs[0]);
    if (v.Kind != UEValueKind::Struct) return false;
    ref = UEStructRef(v);
    for (size_t i = 1; i + 1 < segs.size(); ++i) {
        if (!ref) return false;
        v = ref.GetValue(segs[i]);
        if (v.Kind != UEValueKind::Struct) return false;
        ref = UEStructRef(v);
    }
    if (!ref) return false;
    const std::string& leafName = segs.back();
    UEProperty lp = ref.Find(leafName);
    if (!lp) return false;

    UEValue val = UEValue::MakeString(text);
    std::string tn = lp.GetTypeName();
    if (tn == "FloatProperty" || tn == "DoubleProperty") val = UEValue::MakeFloat(atof(text.c_str()));
    else if (tn == "BoolProperty")                        val = UEValue::MakeBool(text == "true" || text == "1");
    else if (tn.find("IntProperty") != std::string::npos || tn == "ByteProperty") val = UEValue::MakeInt(atoll(text.c_str()));
    else if (tn == "NameProperty")                        val = UEValue::MakeName(text);
    bool ok = false;
    std::string name = leafName;
    UE::DispatchSync([&] { ok = ref.SetValue(name, val); });
    return ok;
}

} // namespace Exec

#endif // UEEDITOR_WITH_BACKEND
