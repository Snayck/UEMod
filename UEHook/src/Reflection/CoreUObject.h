#pragma once

#include "Unreal/UnrealTypes.h"
#include "Engine/Offsets.h"
#include "Core/SafeMemory.h"
#include "Reflection/UEValue.h"
#include <string>
#include <vector>

/*
 * Lightweight handle wrappers over live UObjects in the target process.
 *
 * Each wrapper holds only a raw pointer; copying is free. All member reads go
 * through SafeMemory and the Off:: offset table, so the same code works across
 * the UE versions the offset detector supports.
 */

class UEClass;
class UEStruct;
class UEFunction;
class UEProperty;
class UEEnum;

class UEObject
{
protected:
    uint8* Object = nullptr;

public:
    UEObject() = default;
    UEObject(void* obj) : Object(static_cast<uint8*>(obj)) {}

    inline void* GetAddress() const { return Object; }
    explicit operator bool() const { return Object != nullptr; }
    bool operator==(const UEObject& o) const { return Object == o.Object; }
    bool operator!=(const UEObject& o) const { return Object != o.Object; }

    void*        GetVft() const;
    EObjectFlags GetFlags() const;
    int32        GetIndex() const;
    UEClass      GetClass() const;
    int32        GetNameIndex() const;   // FName ComparisonIndex
    int32        GetNameNumber() const;
    UEObject     GetOuter() const;

    std::string  GetName() const;        // e.g. "BP_Character_C"
    std::string  GetClassName() const;   // class of this object
    std::string  GetFullName() const;    // "Class Outer.Name"
    std::string  GetPathName() const;    // "Outermost.Outer.Name"

    bool IsValid() const;
    bool IsA(UEClass cls) const;
    bool IsA(EClassCastFlags flags) const;
    bool IsA(const std::string& className) const;
    bool IsA(const char* className) const { return IsA(std::string(className ? className : "")); }

    // ---- property access (walks the class hierarchy to find the member) ----
    UEProperty FindProp(const std::string& name) const;
    int32      GetPropOffset(const std::string& name) const;

    template<typename T>
    T Get(const std::string& name) const
    {
        const int32 off = GetPropOffset(name);
        T val{};
        if (off >= 0)
            SafeMemory::Read<T>(reinterpret_cast<uintptr_t>(Object) + off, &val);
        return val;
    }

    template<typename T>
    bool Set(const std::string& name, const T& value)
    {
        const int32 off = GetPropOffset(name);
        if (off < 0 || !Object)
            return false;
        *reinterpret_cast<T*>(Object + off) = value;
        return true;
    }

    // Typed convenience (bool is bitfield-aware).
    bool     GetBool(const std::string& name) const;
    bool     SetBool(const std::string& name, bool value);
    int32    GetInt(const std::string& name) const   { return Get<int32>(name); }
    float    GetFloat(const std::string& name) const { return Get<float>(name); }
    UEObject GetObjectProp(const std::string& name) const;
    std::string GetStringProp(const std::string& name) const; // FString member

    // ---- dynamic, runtime-typed access (for interpreters / node editors) ----
    // Read/write a property by name with the concrete type chosen from
    // reflection, boxed into a UEValue. No compile-time type needed.
    UEValue GetValue(const std::string& name) const;
    bool    SetValue(const std::string& name, const UEValue& value);

    // ---- calling ----
    UEFunction FindFunction(const std::string& name) const; // hierarchy search
    void ProcessEvent(UEFunction func, void* params);
    bool CallFunction(const std::string& name, void* params); // find + ProcessEvent

    // Dynamic call: builds the param frame from the function's reflected params,
    // fills it positionally from `args` (in declaration order, skipping the
    // return value), invokes it, and returns the boxed return value. If `ok` is
    // non-null it receives whether the function was found and called.
    UEValue CallByName(const std::string& funcName,
                       const std::vector<UEValue>& args = {},
                       bool* ok = nullptr);

    template<typename T>
    T Cast() const { return T(Object); }
};

class UEField : public UEObject
{
public:
    using UEObject::UEObject;
    UEField GetNext() const;
};

class UEProperty
{
protected:
    uint8* Prop = nullptr;

public:
    UEProperty() = default;
    UEProperty(void* p) : Prop(static_cast<uint8*>(p)) {}

    inline void* GetAddress() const { return Prop; }
    explicit operator bool() const { return Prop != nullptr; }

    std::string GetName() const;      // member name, e.g. "Health"
    std::string GetTypeName() const;  // e.g. "FloatProperty", "BoolProperty"
    int32 GetOffset() const;
    int32 GetElementSize() const;
    int32 GetArrayDim() const;
    EPropertyFlags GetPropertyFlags() const;
    bool IsType(const std::string& typeName) const;

    // valid only when GetTypeName() == "BoolProperty"
    uint8 GetBoolFieldMask() const;
    int32 GetBoolByteOffset() const;

    UEProperty GetNext() const; // next in the FField/UField chain

    // ---- static type description (for building node pins, no live value) -----
    // Each is meaningful only for the matching property type; otherwise returns
    // an empty handle. These let a node editor describe a pin's target type
    // before any value exists.
    UEStruct   GetStructType() const;    // StructProperty  -> the UScriptStruct
    UEClass    GetObjectClass() const;   // Object/ClassProperty -> target UClass
    UEEnum     GetEnum() const;          // Enum/ByteProperty -> the UEnum
    UEProperty GetInnerProperty() const; // Array/SetProperty -> element property
    UEProperty GetKeyProperty() const;   // MapProperty -> key property
    UEProperty GetValueProperty() const; // MapProperty -> value property

    template<typename T>
    T Cast() const { return T(Prop); }
};

class UEStruct : public UEObject
{
public:
    using UEObject::UEObject;

    UEStruct GetSuper() const;
    void* GetChildren() const;          // UField*  chain head (functions + UProperties)
    void* GetChildProperties() const;   // FField*  chain head (FProperties)
    int32 GetPropertiesSize() const;

    std::vector<UEProperty> GetProperties() const; // this struct only
    std::vector<UEFunction> GetFunctions() const;  // this struct only

    // Hierarchy-flattened (this struct + every super). What a node editor wants
    // to populate a class's full pin/method list.
    std::vector<UEProperty> GetAllProperties() const;
    std::vector<UEFunction> GetAllFunctions() const;

    UEProperty FindMember(const std::string& name) const;   // walks hierarchy
    UEFunction FindFunctionInHierarchy(const std::string& name) const;

    bool IsChildOf(UEStruct parent) const;
};

class UEFunction : public UEStruct
{
public:
    using UEStruct::UEStruct;

    EFunctionFlags GetFunctionFlags() const;
    bool HasFlags(EFunctionFlags flags) const;
    void* GetExecFunction() const;
    int32 GetParmsSize() const;
    uint8 GetNumParms() const;

    // The parameter layout of this function (params + return value), in order.
    // This is the "param structure" -- pair it with a live buffer via UEParams.
    std::vector<UEProperty> GetParams() const;
    UEProperty GetReturnProperty() const; // the ReturnParm property, if any
};

/*
 * Structured, by-name access to a ProcessEvent parameter frame.
 *
 * Holds a (UEFunction, buffer) pair. The buffer is the `void* params` handed to
 * ProcessEvent: each parameter's offset is resolved from the function's reflected
 * properties, so you read/write args and the return value by name without
 * hardcoding a generated struct.
 *
 *     float delta = params.Get<float>("WheelDelta");
 *     params.Set<int32>("Count", 5);
 *     params.SetReturn<bool>(true);   // override the return value
 */
class UEParams
{
    UEFunction Func;
    uint8*     Base = nullptr;

public:
    UEParams() = default;
    UEParams(UEFunction fn, void* base) : Func(fn), Base(static_cast<uint8*>(base)) {}

    explicit operator bool() const { return Base != nullptr; }
    void*      GetAddress() const { return Base; }
    UEFunction GetFunction() const { return Func; }

    UEProperty Find(const std::string& name) const; // param property by name
    int32      OffsetOf(const std::string& name) const;

    template<typename T>
    T Get(const std::string& name) const
    {
        const int32 off = OffsetOf(name);
        T val{};
        if (off >= 0 && Base)
            SafeMemory::Read<T>(reinterpret_cast<uintptr_t>(Base) + off, &val);
        return val;
    }

    template<typename T>
    bool Set(const std::string& name, const T& value)
    {
        const int32 off = OffsetOf(name);
        if (off < 0 || !Base) return false;
        *reinterpret_cast<T*>(Base + off) = value;
        return true;
    }

    // Typed convenience (bool is bitfield-aware).
    bool        GetBool(const std::string& name) const;
    bool        SetBool(const std::string& name, bool value);
    UEObject    GetObject(const std::string& name) const;
    std::string GetString(const std::string& name) const; // FString param

    // Dynamic, runtime-typed access (same as UEObject, over the param frame).
    UEValue GetValue(const std::string& name) const;
    bool    SetValue(const std::string& name, const UEValue& value);

    // Return value (the property flagged ReturnParm).
    template<typename T>
    T GetReturn() const
    {
        UEProperty r = Func.GetReturnProperty();
        T val{};
        if (r && Base)
            SafeMemory::Read<T>(reinterpret_cast<uintptr_t>(Base) + r.GetOffset(), &val);
        return val;
    }

    template<typename T>
    bool SetReturn(const T& value)
    {
        UEProperty r = Func.GetReturnProperty();
        if (!r || !Base) return false;
        *reinterpret_cast<T*>(Base + r.GetOffset()) = value;
        return true;
    }
};

class UEClass : public UEStruct
{
public:
    using UEStruct::UEStruct;

    EClassCastFlags GetCastFlags() const;
    bool IsType(EClassCastFlags flags) const;
    UEObject GetDefaultObject() const;
    UEFunction GetFunction(const std::string& name) const; // hierarchy search
};

/*
 * A UEnum. Exposes its enumerators as (name, value) pairs so a node editor can
 * turn an EnumProperty pin into a dropdown. Get one from UEProperty::GetEnum().
 * The UEnum::Names array offset is resolved lazily from a real enum on first use.
 */
class UEEnum : public UEField
{
public:
    using UEField::UEField;

    // Enumerator (name, value) pairs, in declaration order. Names are as UE
    // stores them (often "EType::Value"); see GetShortNames for the tail only.
    std::vector<std::pair<std::string, int64>> GetNames() const;
    std::vector<std::pair<std::string, int64>> GetShortNames() const; // "Value"

    std::string GetNameByValue(int64 value) const; // enumerator for a value ("" if none)
    int64       GetValueByName(const std::string& name) const; // -1 if not found
};

/*
 * A struct value bound to its type: struct data + the UScriptStruct describing
 * it. Read/write its reflected members by name and enumerate them. Build one
 * from a Struct-kind UEValue (returned by GetValue on a StructProperty). This is
 * the recursive drill-in a node editor needs for nested data.
 *
 *     UEValue t = actor.GetValue("Transform");
 *     UEStructRef tr(t);
 *     UEValue loc = tr.GetValue("Translation");   // itself a Struct
 *     float x = (float)UEStructRef(loc).GetValue("X").AsFloat();
 */
class UEStructRef
{
    uint8*   Base = nullptr;
    UEStruct Type;

public:
    UEStructRef() = default;
    UEStructRef(void* base, void* scriptStruct) : Base(static_cast<uint8*>(base)), Type(scriptStruct) {}
    UEStructRef(const UEValue& v) : Base(static_cast<uint8*>(v.Ptr)), Type(v.AuxPtr) {}

    explicit operator bool() const { return Base != nullptr && Type; }
    void*       GetAddress() const { return Base; }
    UEStruct    GetType() const { return Type; }
    std::string GetTypeName() const;

    std::vector<UEProperty> GetMembers() const; // this struct + supers
    UEProperty Find(const std::string& name) const;

    UEValue GetValue(const std::string& name) const;
    bool    SetValue(const std::string& name, const UEValue& value);
};

/*
 * A view over a live TArray: element type + count + indexed access. Build one
 * from an Array-kind UEValue (returned by GetValue on an ArrayProperty).
 *
 *     UEValue arr = obj.GetValue("Items");
 *     UEArrayRef items(arr);
 *     for (int i = 0; i < items.Num(); ++i)
 *         UEValue e = items.At(i);   // boxed element (scalar, object, struct...)
 */
class UEArrayRef
{
    uint8*     Data = nullptr;
    int32      Count = 0;
    UEProperty Inner;
    int32      Stride = 0;

public:
    UEArrayRef() = default;
    UEArrayRef(const UEValue& v)
        : Data(static_cast<uint8*>(v.Ptr)), Count(static_cast<int32>(v.Int)),
          Inner(v.AuxPtr), Stride(v.Aux) {}

    explicit operator bool() const { return Data != nullptr && Inner; }
    int32       Num() const { return Count; }
    std::string ElementType() const;
    UEValue     At(int32 index) const;
};

/*
 * A view over a live TSet. UE stores a set as an FScriptSet: a sparse array of
 * elements with an allocation bitmask marking which slots are live. Build one
 * from a Set-kind UEValue. Num() is the live element count; At(n) returns the
 * n-th live element (holes from removals are skipped).
 *
 *     UEValue s = obj.GetValue("MySet");
 *     UESetRef set(s);
 *     for (int i = 0; i < set.Num(); ++i) UEValue e = set.At(i);
 */
class UESetRef
{
    uint8*     Base = nullptr; // FScriptSet base (== property address)
    UEProperty Prop;           // the FSetProperty

public:
    UESetRef() = default;
    UESetRef(const UEValue& v) : Base(static_cast<uint8*>(v.Ptr)), Prop(v.AuxPtr) {}

    explicit operator bool() const { return Base != nullptr && Prop; }
    int32       Num() const;          // live element count
    std::string ElementType() const;
    UEValue     At(int32 n) const;    // n-th live element
};

/*
 * A view over a live TMap. A map is an FScriptSet of key/value pairs. Build one
 * from a Map-kind UEValue. Num() is the live pair count; KeyAt(n)/ValueAt(n)
 * return the n-th live pair's key and value.
 *
 *     UEValue m = obj.GetValue("MyMap");
 *     UEMapRef map(m);
 *     for (int i = 0; i < map.Num(); ++i) {
 *         UEValue k = map.KeyAt(i);
 *         UEValue val = map.ValueAt(i);
 *     }
 */
class UEMapRef
{
    uint8*     Base = nullptr; // FScriptMap base (== property address)
    UEProperty Prop;           // the FMapProperty

public:
    UEMapRef() = default;
    UEMapRef(const UEValue& v) : Base(static_cast<uint8*>(v.Ptr)), Prop(v.AuxPtr) {}

    explicit operator bool() const { return Base != nullptr && Prop; }
    int32       Num() const;
    std::string KeyType() const;
    std::string ValueType() const;
    UEValue     KeyAt(int32 n) const;
    UEValue     ValueAt(int32 n) const;
};
