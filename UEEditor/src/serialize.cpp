#include "serialize.h"
#include "customnodes.h"
#include "crude_json.h"
#include <filesystem>
#include <windows.h>

using namespace Editor;

namespace fs = std::filesystem;

namespace {

const char* kPinTypeNames[] = { "Flow", "Bool", "Int", "Float", "String", "Name", "Object", "Struct", "Array", "Any" };
static_assert(IM_ARRAYSIZE(kPinTypeNames) == (int)PinType::Any + 1);

bool PinTypeFromString(const std::string& s, PinType& t) {
    for (int i = 0; i < IM_ARRAYSIZE(kPinTypeNames); ++i)
        if (s == kPinTypeNames[i]) { t = (PinType)i; return true; }
    return false;
}

crude_json::value PinToJson(const Pin& p) {
    crude_json::value j = crude_json::object();
    j["id"]       = (double)(uint32_t)p.ID.Get();
    j["name"]     = p.Name;
    j["type"]     = kPinTypeNames[(int)p.Type];
    j["default"]  = p.DefaultValue;
    j["selectable"] = p.Selectable;
    return j;
}

bool PinFromJson(const crude_json::value& j, Graph& g, std::vector<Pin>& pins, PinKind kind) {
    int id = (int)j["id"].get<double>();
    Pin p(id, j["name"].get<std::string>().c_str(), PinType::Any);
    if (!PinTypeFromString(j["type"].get<std::string>(), p.Type)) return false;
    if (j.contains("default")) p.DefaultValue = j["default"].get<std::string>();
    if (j.contains("selectable")) p.Selectable = j["selectable"].get<bool>();
    p.Kind = kind;
    if (id >= g.NextId) g.NextId = id + 1;
    pins.push_back(std::move(p));
    return true;
}

crude_json::value NodeToJson(const Node& n) {
    crude_json::value j = crude_json::object();
    j["id"]    = (double)(uint32_t)n.ID.Get();
    j["type"]  = n.Type;
    j["name"]  = n.Name;
    j["meta"]  = n.Meta;
    j["color"] = (double)(uint32_t)n.Color;
    j["x"]     = (double)n.Pos.x;
    j["y"]     = (double)n.Pos.y;
    crude_json::value ins = crude_json::array();
    for (const Pin& p : n.Inputs)  ins.push_back(PinToJson(p));
    j["inputs"] = std::move(ins);
    crude_json::value outs = crude_json::array();
    for (const Pin& p : n.Outputs) outs.push_back(PinToJson(p));
    j["outputs"] = std::move(outs);
    return j;
}

void NodeFromJson(const crude_json::value& j, Graph& g) {
    int id = (int)j["id"].get<double>();
    Node& n = g.Nodes.emplace_back(id, j["type"].get<std::string>().c_str());
    if (id >= g.NextId) g.NextId = id + 1;
    if (j.contains("name")) n.Name = j["name"].get<std::string>();
    if (j.contains("meta")) n.Meta = j["meta"].get<std::string>();
    if (j.contains("color")) n.Color = ImColor((ImU32)(uint64_t)j["color"].get<double>());
    if (j.contains("x")) n.Pos = ImVec2((float)j["x"].get<double>(), (float)j["y"].get<double>());
    if (j.contains("inputs"))
        for (const auto& pj : j["inputs"].get<crude_json::array>())
            PinFromJson(pj, g, n.Inputs, PinKind::Input);
    if (j.contains("outputs"))
        for (const auto& pj : j["outputs"].get<crude_json::array>())
            PinFromJson(pj, g, n.Outputs, PinKind::Output);
    for (Pin& p : n.Inputs)  p.Node = &n;
    for (Pin& p : n.Outputs) p.Node = &n;
}

crude_json::value GraphToJson(const Graph& g) {
    crude_json::value j = crude_json::object();
    j["nextId"] = (double)g.NextId;
    crude_json::value nodes = crude_json::array();
    for (const Node& n : g.Nodes) nodes.push_back(NodeToJson(n));
    j["nodes"] = std::move(nodes);
    crude_json::value links = crude_json::array();
    for (const Link& l : g.Links) {
        crude_json::value lj = crude_json::object();
        lj["id"]    = (double)(uint32_t)l.ID.Get();
        lj["start"] = (double)(uint32_t)l.StartPinID.Get();
        lj["end"]   = (double)(uint32_t)l.EndPinID.Get();
        links.push_back(std::move(lj));
    }
    j["links"] = std::move(links);
    crude_json::value vars = crude_json::array();
    {
        std::lock_guard<std::mutex> lock(g.VarsMutex);
        for (const auto& [name, v] : g.Variables) {
            crude_json::value vj = crude_json::object();
            vj["name"] = name;
            vj["type"] = kPinTypeNames[(int)v.Type];
            vj["str"]  = v.Str;
            vj["num"]  = v.Num;
            vj["bool"] = v.Bool;
            vars.push_back(std::move(vj));
        }
    }
    j["vars"] = std::move(vars);
    return j;
}

void GraphFromJson(const crude_json::value& j, Graph& g) {
    g.Nodes.clear();
    g.Links.clear();
    {
        std::lock_guard<std::mutex> lock(g.VarsMutex);
        g.Variables.clear();
    }
    if (j.contains("nextId")) g.NextId = (int)j["nextId"].get<double>();
    if (j.contains("nodes"))
        for (const auto& nj : j["nodes"].get<crude_json::array>())
            NodeFromJson(nj, g);
    if (j.contains("links"))
        for (const auto& lj : j["links"].get<crude_json::array>()) {
            g.Links.emplace_back(g.GetNextId(),
                ed::PinId((uint32_t)(uint64_t)lj["start"].get<double>()),
                ed::PinId((uint32_t)(uint64_t)lj["end"].get<double>()));
        }
    if (j.contains("vars"))
        for (const auto& vj : j["vars"].get<crude_json::array>()) {
            ScriptVar v;
            PinTypeFromString(vj["type"].get<std::string>(), v.Type);
            v.Str  = vj.contains("str")  ? vj["str"].get<std::string>() : "";
            v.Num  = vj.contains("num")  ? vj["num"].get<double>() : 0.0;
            v.Bool = vj.contains("bool") ? vj["bool"].get<bool>() : false;
            std::lock_guard<std::mutex> lock(g.VarsMutex);
            g.Variables[vj["name"].get<std::string>()] = v;
        }
    g.Dirty = false;
}

} // namespace

namespace IO {

std::string BaseDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&BaseDir, &mod);
    WCHAR buf[MAX_PATH] = {};
    GetModuleFileNameW(mod, buf, MAX_PATH);
    fs::path dir = fs::path(buf).parent_path() / "UEEditorFiles";
    std::error_code ec;
    fs::create_directories(dir, ec);
    cached = dir.string();
    return cached;
}

std::string ScriptsDir() {
    std::error_code ec;
    fs::path dir = fs::path(BaseDir()) / "scripts";
    fs::create_directories(dir, ec);
    return dir.string();
}

bool SaveGraph(const Graph& g, const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    std::string data = GraphToJson(g).dump(2);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

bool LoadGraph(Graph& g, const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    fclose(f);

    crude_json::value j = crude_json::value::parse(data);
    if (j.is_null() || !j.is_object()) return false;
    GraphFromJson(j, g);
    return true;
}

bool SaveCustomNodes() {
    crude_json::value arr = crude_json::array();
    for (const CustomNodeDef& d : CustomNodes::All()) {
        crude_json::value dj = crude_json::object();
        dj["name"] = d.Name;
        dj["category"] = d.Category;
        dj["body"] = GraphToJson(d.Body);
        arr.push_back(std::move(dj));
    }
    crude_json::value root = crude_json::object();
    root["nodes"] = std::move(arr);

    std::string path = (fs::path(ScriptsDir()) / "custom_nodes.json").string();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    std::string data = root.dump(2);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

void LoadCustomNodes() {
    std::string path = (fs::path(ScriptsDir()) / "custom_nodes.json").string();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    fclose(f);

    crude_json::value root = crude_json::value::parse(data);
    if (root.is_null() || !root.is_object() || !root.contains("nodes")) return;

    for (const auto& dj : root["nodes"].get<crude_json::array>()) {
        CustomNodeDef d;
        d.Name = dj["name"].get<std::string>();
        d.Category = dj.contains("category") ? dj["category"].get<std::string>() : "Custom";
        GraphFromJson(dj["body"], d.Body);
        if (!d.Body.FindNodeByType("CustomInput") || !d.Body.FindNodeByType("CustomOutput"))
            continue;   // broken def
        CustomNodes::All().push_back(std::move(d));
    }
}

} // namespace IO
