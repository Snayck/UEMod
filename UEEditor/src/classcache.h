#pragma once
#include <string>
#include <vector>

struct FieldDesc {
    std::string Name;
    std::string TypeName;
    bool IsBool = false, IsObject = false, IsStruct = false;
    bool IsArray = false, IsSet = false, IsMap = false, IsEnum = false;
};

struct FuncDesc {
    std::string Name;
    std::string FullName;
};

struct ClassLevel {
    std::string ClassName;
    std::vector<FieldDesc> Fields;
    std::vector<FuncDesc> Functions;
};

struct ClassInfo {
    std::string Name;
    void* UClass = nullptr;
    std::vector<ClassLevel> Levels;      // most-derived first
    std::vector<FieldDesc> FlatFields;
    std::vector<FuncDesc> FlatFunctions;
};

namespace ClassCache {
    inline std::string TrimTypeName(const std::string& tn) {
        if (tn.size() > 8 && tn.compare(tn.size() - 8, 8, "Property") == 0)
            return tn.substr(0, tn.size() - 8);
        return tn;
    }

    const ClassInfo* Get(void* uclass);                          // build on first touch
    const ClassInfo* Find(const std::string& className);         // UE::FindClass + Get
    std::vector<const ClassInfo*> Matches(const std::string& classNameSubstring);
    const FieldDesc* FindField(const ClassInfo* ci, const std::string& fieldName);
    const FuncDesc*  FindFunction(const ClassInfo* ci, const std::string& funcName);
}
