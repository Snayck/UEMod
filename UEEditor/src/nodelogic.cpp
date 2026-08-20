#include "nodelogic.h"

#ifndef UEEDITOR_WITH_BACKEND

// -------- standalone .exe: no backend --------
namespace Exec {
    void InitBackend() {}
    bool BackendReady() { return false; }
    const char* BackendStatus() { return "standalone (no backend)"; }
    void RunScriptNode(Editor::Graph&, Editor::Node&) {}
    void RunGraph(Editor::Graph&) {}
    void TickFrame(Editor::Graph&) {}
}

#else
// -------- injected build --------
#include "imgui.h"
#include "UEHook.h"
#include <chrono>
#include <string>
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
    explicit Ctx(Graph& gg) : g(gg) {}

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
                NodeLogic* logic = LogicFor(src->Node->Name);
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
                if (dst->Node) LogicFor(dst->Node->Name)->Execute(*this, *dst->Node);
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

    static const std::unordered_map<std::string, NodeLogic*> map = {
        { "GetWorld", &getWorld }, { "FindObject", &findObject }, { "GetObjectsOfClass", &getObjects },
        { "GetValue", &getValue }, { "GetName", &getName }, { "GetClassName", &getClassName },
        { "GetFullName", &getFullName }, { "IsValid", &isValid }, { "IsA", &isA },
        { "GetAllFields", &getFields }, { "GetAllMethods", &getMethods },
        { "ArrayLength", &arrLen }, { "ArrayGet", &arrGet }, { "Literal", &literal },
        { "Add", &math }, { "Subtract", &math }, { "Multiply", &math }, { "Divide", &math },
        { "A > B", &math }, { "A < B", &math }, { "A == B", &math }, { "And", &math }, { "Or", &math },
        { "Not", &notLogic }, { "GetParam", &getParam },
        { "Sequence", &sequence }, { "Branch", &branch }, { "ForEach", &forEach },
        { "ForLoop", &forLoop }, { "WhileLoop", &whileLoop },
        { "SetValue", &setValue }, { "CallFunction", &callFn }, { "Print", &print },
        { "DrawText", &drawText }, { "DrawLine", &drawLine },
    };
    auto it = map.find(name);
    return it != map.end() ? it->second : &base;
}

void RegisterHook(Graph& g, Node& node, bool post) {
    Pin* fnPin = InPin(node, "Function");
    std::string fn = fnPin ? fnPin->DefaultValue : "";
    if (fn.empty()) {
        FailNode(node, "Function: not set");
        return;
    }
    Node* np = &node;
    Graph* gp = &g;
    auto cb = [gp, np, post](Hooking::HookContext& hc) {
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
}

} // namespace

namespace Exec {

void InitBackend() {
    UEHookConfig cfg;
    cfg.verbose = true;
    UE::Initialize(cfg);
}

bool BackendReady() { return UE::IsInitialized(); }

const char* BackendStatus()
{
    return UE::IsInitialized() ? "ready" : "init failed";
}

void RunGraph(Graph& g) {
    if (!UE::IsInitialized()) {
        printf("[RunGraph] backend not initialized\n");
        return;
    }
    for (Node& n : g.Nodes) {
        if (n.Name == "PreHook")  RegisterHook(g, n, false);
        if (n.Name == "PostHook") RegisterHook(g, n, true);
    }
}

void RunScriptNode(Graph& g, Node& n) {
    if (!UE::IsInitialized()) {
        FailNode(n, "backend not initialized (see Control window)");
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
        if (n.Name == "OnFrameRender") ctx.RunNext(n);
}

} // namespace Exec

#endif // UEEDITOR_WITH_BACKEND
