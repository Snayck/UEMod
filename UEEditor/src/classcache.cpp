#include "classcache.h"

#ifndef UEEDITOR_WITH_BACKEND

namespace ClassCache {
    const ClassInfo* Get(void*) { return nullptr; }
    const ClassInfo* Find(const std::string&) { return nullptr; }
    std::vector<const ClassInfo*> Matches(const std::string&) { return {}; }
    const FieldDesc* FindField(const ClassInfo*, const std::string&) { return nullptr; }
    const FuncDesc* FindFunction(const ClassInfo*, const std::string&) { return nullptr; }
}

#else

#include "UEHook.h"
#include <unordered_map>

namespace ClassCache {

namespace {
    std::unordered_map<void*, ClassInfo> g_Cache;

    FieldDesc MakeField(const UEProperty& p) {
        FieldDesc f;
        f.Name = p.GetName();
        std::string raw = p.GetTypeName();
        f.TypeName = TrimTypeName(raw);
        f.IsBool   = raw == "BoolProperty";
        f.IsObject = raw.find("ObjectProperty") != std::string::npos
                  || raw.find("ClassProperty") != std::string::npos;
        f.IsStruct = raw == "StructProperty";
        f.IsArray  = raw == "ArrayProperty";
        f.IsSet    = raw == "SetProperty";
        f.IsMap    = raw == "MapProperty";
        f.IsEnum   = raw == "EnumProperty" || raw == "ByteProperty";
        return f;
    }

    const ClassInfo* Build(UEStruct s) {
        ClassInfo ci;
        ci.UClass = s.GetAddress();
        ci.Name = s.GetName();

        int guard = 0;
        for (UEStruct lvl = s; lvl && guard++ < 64; lvl = lvl.GetSuper()) {
            ClassLevel cl;
            cl.ClassName = lvl.GetName();
            for (const UEProperty& p : lvl.GetProperties()) {
                cl.Fields.push_back(MakeField(p));
                ci.FlatFields.push_back(MakeField(p));
            }
            for (const UEFunction& fn : lvl.GetFunctions()) {
                FuncDesc fd;
                fd.Name = fn.GetName();
                fd.FullName = fn.GetFullName();
                cl.Functions.push_back(std::move(fd));
                ci.FlatFunctions.push_back(cl.Functions.back());
            }
            ci.Levels.push_back(std::move(cl));
        }

        auto [it, inserted] = g_Cache.emplace(ci.UClass, std::move(ci));
        return &it->second;
    }
}

const ClassInfo* Get(void* uclass) {
    if (!uclass) return nullptr;
    auto it = g_Cache.find(uclass);
    if (it != g_Cache.end()) return &it->second;
    return Build(UEClass(uclass));
}

const ClassInfo* Find(const std::string& className) {
    UEClass c = UE::FindClass(className);
    return c ? Get(c.GetAddress()) : nullptr;
}

std::vector<const ClassInfo*> Matches(const std::string& classNameSubstring) {
    std::vector<const ClassInfo*> out;
    std::string filter = classNameSubstring;
    for (char& c : filter) if (c >= 'A' && c <= 'Z') c += 32;
    if (filter.empty()) return out;
    for (UEClass c : UE::GetAllClasses(filter))
        if (const ClassInfo* ci = Get(c.GetAddress()))
            out.push_back(ci);
    return out;
}

const FieldDesc* FindField(const ClassInfo* ci, const std::string& fieldName) {
    if (!ci) return nullptr;
    for (const FieldDesc& f : ci->FlatFields)
        if (f.Name == fieldName) return &f;
    return nullptr;
}

const FuncDesc* FindFunction(const ClassInfo* ci, const std::string& funcName) {
    if (!ci) return nullptr;
    for (const FuncDesc& f : ci->FlatFunctions)
        if (f.Name == funcName) return &f;
    return nullptr;
}

} // namespace ClassCache

#endif
