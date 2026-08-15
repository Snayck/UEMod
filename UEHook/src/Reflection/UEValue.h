#pragma once

#include "Unreal/UnrealTypes.h"
#include <string>
#include <cstdint>
#include <utility>

/*
 * UEValue - a type-erased, runtime-typed value.
 *
 * The templated UEObject::Get<T>/Set<T> API needs the type at compile time. A
 * node editor (or any interpreter) doesn't have that -- it only knows a property
 * name and, from reflection, its type name. UEValue is the boxed value that
 * flows across that boundary: read a property to a UEValue, or write a UEValue
 * back, with the concrete conversion chosen from the property's reflected type.
 */

class UEObject; // forward (defined in CoreUObject.h)

enum class UEValueKind
{
    None,
    Bool,
    Int,     // any signed/unsigned integer, byte, or enum (stored in Int)
    Float,   // float or double (stored in Float)
    String,  // FString / FText
    Name,    // FName (Str = text, Int = comparison index)
    Object,  // UObject* (Ptr)
    Struct,  // in-place struct (Ptr = base, AuxPtr = UScriptStruct*, Str = type)
    Array,   // TArray (Ptr = data, Int = Num, AuxPtr = inner FProperty*, Aux = stride)
    Set,     // TSet (Ptr = FScriptSet base, AuxPtr = FSetProperty*, Str = elem type)
    Map      // TMap (Ptr = FScriptMap base, AuxPtr = FMapProperty*, Str = "K -> V")
};

class UEValue
{
public:
    UEValueKind Kind = UEValueKind::None;

    int64_t     Int   = 0;
    double      Float = 0.0;
    std::string Str;
    void*       Ptr   = nullptr;
    std::string TypeName; // reflected property/struct type, for round-tripping

    // Auxiliary data for the compound kinds (Struct / Array). For Struct: AuxPtr
    // is the UScriptStruct*. For Array: AuxPtr is the inner FProperty* and Aux is
    // the element stride. Use UEStructRef / UEArrayRef (CoreUObject.h) to drill in.
    void*   AuxPtr = nullptr;
    int32_t Aux    = 0;

    UEValue() = default;

    // ---- factories ----
    static UEValue MakeBool(bool b)             { UEValue v; v.Kind = UEValueKind::Bool;  v.Int = b ? 1 : 0; return v; }
    static UEValue MakeInt(int64_t i)           { UEValue v; v.Kind = UEValueKind::Int;   v.Int = i;         return v; }
    static UEValue MakeFloat(double f)          { UEValue v; v.Kind = UEValueKind::Float; v.Float = f;       return v; }
    static UEValue MakeString(std::string s)    { UEValue v; v.Kind = UEValueKind::String; v.Str = std::move(s); return v; }
    static UEValue MakeName(std::string s, int64_t idx = 0)
                                                { UEValue v; v.Kind = UEValueKind::Name;  v.Str = std::move(s); v.Int = idx; return v; }
    static UEValue MakeObject(void* p)          { UEValue v; v.Kind = UEValueKind::Object; v.Ptr = p;        return v; }
    static UEValue MakeStruct(void* base, std::string type, void* scriptStruct = nullptr)
                                                { UEValue v; v.Kind = UEValueKind::Struct; v.Ptr = base; v.Str = std::move(type); v.TypeName = v.Str; v.AuxPtr = scriptStruct; return v; }
    static UEValue MakeArray(void* data, int32_t num, void* inner, int32_t stride, std::string elemType)
                                                { UEValue v; v.Kind = UEValueKind::Array; v.Ptr = data; v.Int = num; v.AuxPtr = inner; v.Aux = stride; v.Str = std::move(elemType); v.TypeName = v.Str; return v; }
    static UEValue MakeSet(void* setBase, void* setProp, std::string elemType)
                                                { UEValue v; v.Kind = UEValueKind::Set; v.Ptr = setBase; v.AuxPtr = setProp; v.Str = std::move(elemType); v.TypeName = v.Str; return v; }
    static UEValue MakeMap(void* mapBase, void* mapProp, std::string kvType)
                                                { UEValue v; v.Kind = UEValueKind::Map; v.Ptr = mapBase; v.AuxPtr = mapProp; v.Str = std::move(kvType); v.TypeName = v.Str; return v; }

    // Convenience ctors so callers can pass literals as args.
    UEValue(bool b)        : Kind(UEValueKind::Bool),  Int(b ? 1 : 0) {}
    UEValue(int32 i)       : Kind(UEValueKind::Int),   Int(i) {}
    UEValue(int64_t i)     : Kind(UEValueKind::Int),   Int(i) {}
    UEValue(float f)       : Kind(UEValueKind::Float), Float(f) {}
    UEValue(double f)      : Kind(UEValueKind::Float), Float(f) {}
    UEValue(const char* s) : Kind(UEValueKind::String), Str(s ? s : "") {}
    UEValue(std::string s) : Kind(UEValueKind::String), Str(std::move(s)) {}

    // ---- queries ----
    bool IsNone()   const { return Kind == UEValueKind::None; }
    explicit operator bool() const { return Kind != UEValueKind::None; }

    // ---- coercing accessors (best-effort across kinds) ----
    bool AsBool() const
    {
        switch (Kind)
        {
        case UEValueKind::Bool:
        case UEValueKind::Int:    return Int != 0;
        case UEValueKind::Float:  return Float != 0.0;
        case UEValueKind::Object:
        case UEValueKind::Struct: return Ptr != nullptr;
        default: return false;
        }
    }
    int64_t AsInt() const
    {
        if (Kind == UEValueKind::Float) return static_cast<int64_t>(Float);
        return Int;
    }
    double AsFloat() const
    {
        if (Kind == UEValueKind::Int || Kind == UEValueKind::Bool) return static_cast<double>(Int);
        return Float;
    }
    const std::string& AsString() const { return Str; }
    void* AsObject() const { return Ptr; }

    std::string ToString() const; // human-readable, for logging / node display
};
