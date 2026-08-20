#include "CoreUObject.h"
#include "Unreal/NameArray.h"
#include <Windows.h>
#include <cstdio>

// ---- small local read helpers -------------------------------------------------
namespace
{
    template<typename T>
    T ReadAt(const uint8* base, int32 offset)
    {
        T val{};
        if (base)
            SafeMemory::Read<T>(reinterpret_cast<uintptr_t>(base) + offset, &val);
        return val;
    }

    inline void* ReadPtr(const uint8* base, int32 offset)
    {
        return reinterpret_cast<void*>(ReadAt<uintptr_t>(base, offset));
    }

    // Integer property type -> (byte width, is-signed). ok=false if not an int.
    struct IntSpec { int bytes; bool sign; bool ok; };
    IntSpec IntSpecFor(const std::string& t)
    {
        if (t == "IntProperty")    return { 4, true,  true };
        if (t == "Int64Property")  return { 8, true,  true };
        if (t == "Int16Property")  return { 2, true,  true };
        if (t == "Int8Property")   return { 1, true,  true };
        if (t == "ByteProperty")   return { 1, false, true };
        if (t == "UInt16Property") return { 2, false, true };
        if (t == "UInt32Property") return { 4, false, true };
        if (t == "UInt64Property") return { 8, false, true };
        return { 0, false, false };
    }

    bool IsObjectType(const std::string& t)
    {
        return t == "ObjectProperty" || t == "ClassProperty" ||
               t == "InterfaceProperty";
    }

    bool EndsWith(const std::string& s, const char* suffix)
    {
        const size_t n = std::char_traits<char>::length(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    // Does this pointer look like an FField (its Class resolves to a *Property)?
    bool LooksLikeFField(void* f)
    {
        if (!f || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(f)))
            return false;
        void* fc = ReadPtr(static_cast<uint8*>(f), Off::FField::Class);
        if (!fc || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(fc)))
            return false;
        int32 idx = 0;
        SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(fc), &idx);
        return EndsWith(NameArray::GetNameByIndex(idx), "Property");
    }

    // Does this pointer look like a UScriptStruct (class name == "ScriptStruct")?
    bool LooksLikeScriptStruct(void* o)
    {
        if (!o || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(o)))
            return false;
        return UEObject(o).GetClassName() == "ScriptStruct";
    }

    // Does this pointer look like a UClass / UEnum (by its class name)?
    bool LooksLikeUObjectOfClass(void* o, const char* className)
    {
        if (!o || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(o)))
            return false;
        return UEObject(o).GetClassName() == className;
    }

    // The FFieldVariant owner ptr of an FField (its containing class/struct/prop).
    uintptr_t OwnerOf(void* field)
    {
        return reinterpret_cast<uintptr_t>(ReadPtr(static_cast<uint8*>(field), Off::FField::Owner));
    }

    // A concrete property's extra members (StructProperty::Struct,
    // ArrayProperty::Inner, ...) begin at sizeof(FProperty) == FBoolProperty::Base.
    // We scan a small window around it: the exact size varies slightly by build,
    // and it lets us skip the FProperty link pointers that also look like FFields.
    template<typename Pred>
    void* ScanSubPointer(uint8* prop, Pred pred)
    {
        const int base = Off::FBoolProperty::Base;
        for (int o = base; o <= base + 0x18; o += 8)
        {
            void* q = ReadPtr(prop, o);
            if (pred(q)) return q;
        }
        for (int o = base - 0x18; o < base; o += 8) // widen downward as a fallback
        {
            if (o < 0x28) continue;
            void* q = ReadPtr(prop, o);
            if (pred(q)) return q;
        }
        return nullptr;
    }

    // Collect every sub-FField owned by this property (its Owner points back to
    // it), in ascending address order -- e.g. a map's {KeyProp, ValueProp}.
    std::vector<void*> ScanSubFields(uint8* prop, int maxCount)
    {
        std::vector<void*> out;
        const uintptr_t self = reinterpret_cast<uintptr_t>(prop);
        const int base = Off::FBoolProperty::Base;
        for (int o = base; o <= base + 0x20 && (int)out.size() < maxCount; o += 8)
        {
            void* q = ReadPtr(prop, o);
            if (LooksLikeFField(q) && OwnerOf(q) == self)
                out.push_back(q);
        }
        return out;
    }

    // ---- FScriptSet / FScriptSparseArray access ------------------------------
    // Layout (x64): FScriptSparseArray { void* Data@0; int32 Num@8; int32 Max@0xC;
    //   FScriptBitArray AllocationFlags@0x10 { uint32 Inline[4]@0x10; void* Heap@0x20;
    //   int32 NumBits@0x28; int32 MaxBits@0x2C }; int32 FirstFree@0x30; int32 NumFree@0x34 }.
    // A set/map property's data *is* the FScriptSparseArray (the hash follows it).
    constexpr int kSparseInlineBits = 128; // 4 * 32 (TInlineAllocator<4> of uint32)

    // The allocation bitmap base + allocated-slot count. The bitmap is the single
    // source of truth for which slots are live -- NumFreeIndices can't be trusted
    // to agree with it, so we derive both count and iteration from the bits.
    struct SparseInfo { uintptr_t bits; int32 total; };

    SparseInfo SparseInfoOf(uint8* setBase)
    {
        int32 total = ReadAt<int32>(setBase, 0x08); // FScriptArray::ArrayNum (incl. holes)
        if (total < 0 || total > 0x1000000) total = 0;
        int32 maxBits = ReadAt<int32>(setBase, 0x2C);
        uintptr_t bits = (maxBits <= kSparseInlineBits)
                             ? reinterpret_cast<uintptr_t>(setBase) + 0x10       // inline
                             : reinterpret_cast<uintptr_t>(ReadPtr(setBase, 0x20)); // heap
        return { bits, total };
    }

    bool SparseBitSet(uintptr_t bits, int32 i)
    {
        if (!bits) return false;
        uint32 word = 0;
        SafeMemory::Read<uint32>(bits + (i / 32) * 4, &word);
        return (word >> (i % 32)) & 1u;
    }

    // Map slot index n (n-th *live* element) to its sparse slot, or -1.
    int32 SparseNthLive(uint8* setBase, int32 n)
    {
        SparseInfo si = SparseInfoOf(setBase);
        if (!si.bits) return -1;
        int32 seen = 0;
        for (int32 i = 0; i < si.total; ++i)
        {
            if (!SparseBitSet(si.bits, i)) continue;
            if (seen == n) return i;
            ++seen;
        }
        return -1;
    }

    int32 SparseLiveCount(uint8* setBase)
    {
        SparseInfo si = SparseInfoOf(setBase);
        if (!si.bits) return 0;
        int32 live = 0;
        for (int32 i = 0; i < si.total; ++i)
            if (SparseBitSet(si.bits, i)) ++live;
        return live;
    }

    // Derived element geometry of a set/map. Set: KeyProp only, ValueOffset unused.
    struct ContainerLayout { int32 stride; int32 keyOff; int32 valOff; };

    int32 RoughAlign(int32 size) { return (size % 8 == 0 && size >= 8) ? 8 : 4; }

    ContainerLayout SetLayoutFor(const UEProperty& elem)
    {
        const int32 es = elem.GetElementSize() > 0 ? elem.GetElementSize() : 8;
        // TSetElement<T> = { T Value; int32 HashNextId; int32 HashIndex }.
        int32 payload = Align(es, 4) + 8;
        int32 stride  = Align(payload, RoughAlign(es));
        return { stride, 0, 0 };
    }

    // ---- UEnum::Names resolution ---------------------------------------------
    // Names is a TArray<TPair<FName,int64>>: stride 0x10, FName idx@+0, value@+8.
    // Validate a candidate offset by decoding the first enumerator's FName.
    bool ValidEnumNamesAt(uint8* enumObj, int off)
    {
        if (off < 0x18) return false;
        void* data = ReadPtr(enumObj, off);
        int32 num  = ReadAt<int32>(enumObj, off + sizeof(void*));
        if (!data || num < 1 || num > 0x4000) return false;
        if (!SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(data))) return false;
        int32 idx = 0;
        if (!SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(data), &idx)) return false;
        std::string s = NameArray::GetNameByIndex(idx);
        return !s.empty() && s.size() < 200;
    }

    // Resolve (and cache) the Names offset against this real enum object.
    int ResolveEnumNamesOffset(uint8* enumObj)
    {
        if (ValidEnumNamesAt(enumObj, Off::UEnum::Names))
            return Off::UEnum::Names;
        for (int o = 0x28; o <= 0x88; o += 8)
        {
            if (ValidEnumNamesAt(enumObj, o))
            {
                Off::UEnum::Names = o; // cache for next time
                return o;
            }
        }
        return -1;
    }

    ContainerLayout MapLayoutFor(const UEProperty& key, const UEProperty& val)
    {
        const int32 ks = key.GetElementSize() > 0 ? key.GetElementSize() : 8;
        const int32 vs = val.GetElementSize() > 0 ? val.GetElementSize() : 8;
        const int32 vAlign = RoughAlign(vs);
        const int32 valOff = Align(ks, vAlign);
        const int32 pairSize  = valOff + vs;
        const int32 pairAlign = RoughAlign(ks) > vAlign ? RoughAlign(ks) : vAlign;
        // sparse element = { TPair<K,V>; int32 HashNextId; int32 HashIndex }.
        int32 payload = Align(pairSize, 4) + 8;
        int32 stride  = Align(payload, pairAlign);
        return { stride, 0, valOff };
    }

    int64_t ReadIntWidth(uintptr_t addr, int bytes, bool sign)
    {
        switch (bytes)
        {
        case 1: { uint8  v = 0; SafeMemory::Read<uint8>(addr, &v);  return sign ? (int64_t)(int8) v  : (int64_t)v; }
        case 2: { uint16 v = 0; SafeMemory::Read<uint16>(addr, &v); return sign ? (int64_t)(int16)v  : (int64_t)v; }
        case 4: { uint32 v = 0; SafeMemory::Read<uint32>(addr, &v); return sign ? (int64_t)(int32)v  : (int64_t)v; }
        case 8: { uint64 v = 0; SafeMemory::Read<uint64>(addr, &v); return (int64_t)v; }
        }
        return 0;
    }

    void WriteIntWidth(uintptr_t addr, int bytes, int64_t val)
    {
        switch (bytes)
        {
        case 1: *reinterpret_cast<uint8*>(addr)  = (uint8)val;  break;
        case 2: *reinterpret_cast<uint16*>(addr) = (uint16)val; break;
        case 4: *reinterpret_cast<uint32*>(addr) = (uint32)val; break;
        case 8: *reinterpret_cast<uint64*>(addr) = (uint64)val; break;
        }
    }

    std::string ReadFStringAt(uintptr_t addr)
    {
        // FString == TArray<wchar_t>: { wchar_t* Data; int32 Num; int32 Max; }
        uintptr_t data = 0;
        SafeMemory::Read<uintptr_t>(addr, &data);
        int32 num = 0;
        SafeMemory::Read<int32>(addr + sizeof(void*), &num);
        if (!data || num <= 0 || num > 0x100000)
            return "";

        std::string out;
        const int n = num < 1023 ? num : 1023;
        for (int i = 0; i < n; ++i)
        {
            wchar_t c = 0;
            if (!SafeMemory::Read<wchar_t>(data + i * sizeof(wchar_t), &c) || !c)
                break;
            out += (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
        }
        return out;
    }

    // Read one property out of a buffer/object into a boxed UEValue.
    UEValue ReadProperty(const UEProperty& p, uint8* base)
    {
        if (!p || !base) return {};
        const uintptr_t addr = reinterpret_cast<uintptr_t>(base) + p.GetOffset();
        const std::string t = p.GetTypeName();

        if (t == "BoolProperty")
        {
            uint8 byte = ReadAt<uint8>(base, p.GetOffset() + p.GetBoolByteOffset());
            return UEValue::MakeBool((byte & p.GetBoolFieldMask()) != 0);
        }
        if (t == "FloatProperty")  { float f = 0;  SafeMemory::Read<float>(addr, &f);  return UEValue::MakeFloat(f); }
        if (t == "DoubleProperty") { double d = 0; SafeMemory::Read<double>(addr, &d); return UEValue::MakeFloat(d); }

        if (t == "EnumProperty")
        {
            const int w = p.GetElementSize() > 0 ? p.GetElementSize() : 1;
            return UEValue::MakeInt(ReadIntWidth(addr, w, false));
        }
        if (IntSpec s = IntSpecFor(t); s.ok)
            return UEValue::MakeInt(ReadIntWidth(addr, s.bytes, s.sign));

        if (t == "NameProperty")
        {
            int32 idx = ReadAt<int32>(base, p.GetOffset());
            return UEValue::MakeName(NameArray::GetNameByIndex(idx), idx);
        }
        if (t == "StrProperty")  return UEValue::MakeString(ReadFStringAt(addr));

        if (IsObjectType(t))     return UEValue::MakeObject(ReadPtr(base, p.GetOffset()));

        if (t == "StructProperty")
        {
            // FStructProperty::Struct: the UScriptStruct near sizeof(FProperty).
            uint8* pp = static_cast<uint8*>(p.GetAddress());
            void* ss = ScanSubPointer(pp, [](void* q) { return LooksLikeScriptStruct(q); });
            std::string tn = ss ? UEObject(ss).GetName() : "";
            return UEValue::MakeStruct(reinterpret_cast<void*>(addr), tn, ss);
        }

        if (t == "ArrayProperty")
        {
            // FArrayProperty::Inner: the FField near sizeof(FProperty) whose Owner
            // points back to this property (distinguishes it from FProperty's own
            // PropertyLinkNext/NextRef pointers, which also look like FFields).
            uint8* pp = static_cast<uint8*>(p.GetAddress());
            const uintptr_t self = reinterpret_cast<uintptr_t>(pp);
            void* inner = ScanSubPointer(pp, [&](void* q)
                { return LooksLikeFField(q) && OwnerOf(q) == self; });
            if (!inner) return {};

            void* data = ReadPtr(base, p.GetOffset());              // TArray::Data
            int32 num  = ReadAt<int32>(base, p.GetOffset() + sizeof(void*)); // ::Num
            UEProperty ip(inner);
            const int32 stride = ip.GetElementSize();
            if (stride <= 0) return {};
            if (num < 0 || num > 0x1000000) num = 0;
            return UEValue::MakeArray(data, num, inner, stride, ip.GetTypeName());
        }

        if (t == "SetProperty")
        {
            uint8* pp = static_cast<uint8*>(p.GetAddress());
            std::vector<void*> fields = ScanSubFields(pp, 1);
            if (fields.empty()) return {};
            UEProperty elem(fields[0]);
            return UEValue::MakeSet(reinterpret_cast<void*>(addr), pp, elem.GetTypeName());
        }

        if (t == "MapProperty")
        {
            uint8* pp = static_cast<uint8*>(p.GetAddress());
            std::vector<void*> fields = ScanSubFields(pp, 2);
            if (fields.size() < 2) return {};
            UEProperty kp(fields[0]), vp(fields[1]);
            const std::string kv = kp.GetTypeName() + " -> " + vp.GetTypeName();
            return UEValue::MakeMap(reinterpret_cast<void*>(addr), pp, kv);
        }

        return {}; // TextProperty, delegates, etc. -> None for now
    }

    // Copy a std::string's chars into a wide buffer (ASCII/latin-1, null-terminated).
    void FillWide(wchar_t* dst, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
        dst[s.size()] = 0;
    }

    // Write an FString ({wchar_t* Data; int32 Num; int32 Max}, counts include the
    // null terminator) at `addr` from `s`.
    //   allocSink != null  -> transient frame: allocate a fresh buffer and record
    //                         it so the caller frees it after the call returns.
    //   allocSink == null   -> persistent property: overwrite in place only if the
    //                         existing allocation is large enough (growing would
    //                         need the game's own allocator, which we don't call).
    bool WriteFString(uintptr_t addr, const std::string& s, std::vector<void*>* allocSink)
    {
        const int32 need = static_cast<int32>(s.size()) + 1; // incl. null

        if (allocSink)
        {
            wchar_t* buf = new wchar_t[static_cast<size_t>(need)];
            FillWide(buf, s);
            *reinterpret_cast<void**>(addr)          = buf;
            *reinterpret_cast<int32*>(addr + 8)      = need;
            *reinterpret_cast<int32*>(addr + 12)     = need;
            allocSink->push_back(buf);
            return true;
        }

        void* data  = *reinterpret_cast<void**>(addr);
        int32 maxEl = *reinterpret_cast<int32*>(addr + 12);
        if (data && maxEl >= need && SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(data)))
        {
            FillWide(reinterpret_cast<wchar_t*>(data), s);
            *reinterpret_cast<int32*>(addr + 8) = need;
            return true;
        }
        return false; // can't grow a live FString without the engine allocator
    }

    // Write a boxed UEValue into a buffer/object property. `allocSink`, when
    // provided (a transient ProcessEvent frame), collects heap buffers the caller
    // must free after the call. Returns false for types we can't write yet.
    bool WriteProperty(const UEProperty& p, uint8* base, const UEValue& v,
                       std::vector<void*>* allocSink = nullptr)
    {
        if (!p || !base) return false;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(base) + p.GetOffset();
        const std::string t = p.GetTypeName();

        if (t == "BoolProperty")
        {
            uint8* byte = base + p.GetOffset() + p.GetBoolByteOffset();
            const uint8 mask = p.GetBoolFieldMask();
            *byte = v.AsBool() ? (*byte | mask) : (*byte & ~mask);
            return true;
        }
        if (t == "FloatProperty")  { *reinterpret_cast<float*>(addr)  = (float)v.AsFloat();  return true; }
        if (t == "DoubleProperty") { *reinterpret_cast<double*>(addr) = v.AsFloat();         return true; }

        if (t == "EnumProperty")
        {
            const int w = p.GetElementSize() > 0 ? p.GetElementSize() : 1;
            WriteIntWidth(addr, w, v.AsInt());
            return true;
        }
        if (IntSpec s = IntSpecFor(t); s.ok) { WriteIntWidth(addr, s.bytes, v.AsInt()); return true; }

        if (IsObjectType(t)) { *reinterpret_cast<void**>(addr) = v.AsObject(); return true; }

        if (t == "StrProperty")
            return WriteFString(addr, v.AsString(), allocSink);

        if (t == "NameProperty")
        {
            // Prefer an explicit comparison index; otherwise resolve the text
            // against the name pool (only names that already exist can be set).
            int32 idx = -1;
            if (v.Kind == UEValueKind::Name && v.Int != 0)
                idx = static_cast<int32>(v.Int);
            else if (!v.Str.empty())
                idx = NameArray::FindIndex(v.Str);
            if (idx < 0) return false;
            *reinterpret_cast<int32*>(addr)     = idx; // comparison index
            *reinterpret_cast<int32*>(addr + 4) = 0;   // number
            return true;
        }
        return false;
    }
}

std::string UEValue::ToString() const
{
    switch (Kind)
    {
    case UEValueKind::None:   return "None";
    case UEValueKind::Bool:   return Int ? "true" : "false";
    case UEValueKind::Int:    return std::to_string(Int);
    case UEValueKind::Float:  { char b[32]; std::snprintf(b, sizeof(b), "%g", Float); return b; }
    case UEValueKind::String: return Str;
    case UEValueKind::Name:   return Str;
    case UEValueKind::Object:
    {
        if (!Ptr) return "None";
        std::string n = UEObject(Ptr).GetName();
        return n.empty() ? "<object>" : n;
    }
    case UEValueKind::Struct: return Str.empty() ? "<struct>" : ("<" + Str + ">");
    case UEValueKind::Array:  return "[" + std::to_string(Int) + " x " + (Str.empty() ? "?" : Str) + "]";
    case UEValueKind::Set:    return "set{" + (Str.empty() ? "?" : Str) + "}";
    case UEValueKind::Map:    return "map{" + (Str.empty() ? "?" : Str) + "}";
    }
    return "";
}

// ============================ UEStructRef ====================================

std::string UEStructRef::GetTypeName() const
{
    return Type ? UEObject(Type.GetAddress()).GetName() : "";
}

std::vector<UEProperty> UEStructRef::GetMembers() const
{
    std::vector<UEProperty> out;
    int guard = 0;
    for (UEStruct s = Type; s && guard++ < 64; s = s.GetSuper())
    {
        for (const UEProperty& p : s.GetProperties())
            out.push_back(p);
    }
    return out;
}

UEProperty UEStructRef::Find(const std::string& name) const
{
    return Type ? Type.FindMember(name) : UEProperty();
}

UEValue UEStructRef::GetValue(const std::string& name) const
{
    return ReadProperty(Find(name), Base);
}

bool UEStructRef::SetValue(const std::string& name, const UEValue& value)
{
    return WriteProperty(Find(name), Base, value);
}

// ============================ UEArrayRef =====================================

std::string UEArrayRef::ElementType() const
{
    return Inner ? Inner.GetTypeName() : "";
}

UEValue UEArrayRef::At(int32 index) const
{
    if (!Data || !Inner || index < 0 || index >= Count)
        return {};
    return ReadProperty(Inner, Data + static_cast<size_t>(index) * Stride);
}

// ============================ UESetRef =======================================

int32 UESetRef::Num() const
{
    return Base ? SparseLiveCount(Base) : 0;
}

std::string UESetRef::ElementType() const
{
    std::vector<void*> f = ScanSubFields(static_cast<uint8*>(Prop.GetAddress()), 1);
    return f.empty() ? "" : UEProperty(f[0]).GetTypeName();
}

UEValue UESetRef::At(int32 n) const
{
    if (!Base || !Prop || n < 0) return {};
    std::vector<void*> f = ScanSubFields(static_cast<uint8*>(Prop.GetAddress()), 1);
    if (f.empty()) return {};
    UEProperty elem(f[0]);
    const ContainerLayout lay = SetLayoutFor(elem);

    void* data = ReadPtr(Base, 0x00);
    if (!data) return {};
    int32 slot = SparseNthLive(Base, n);
    if (slot < 0) return {};
    uint8* elemAddr = static_cast<uint8*>(data) + static_cast<size_t>(slot) * lay.stride;
    return ReadProperty(elem, elemAddr);
}

// ============================ UEMapRef =======================================

int32 UEMapRef::Num() const
{
    return Base ? SparseLiveCount(Base) : 0;
}

std::string UEMapRef::KeyType() const
{
    std::vector<void*> f = ScanSubFields(static_cast<uint8*>(Prop.GetAddress()), 2);
    return f.size() < 1 ? "" : UEProperty(f[0]).GetTypeName();
}

std::string UEMapRef::ValueType() const
{
    std::vector<void*> f = ScanSubFields(static_cast<uint8*>(Prop.GetAddress()), 2);
    return f.size() < 2 ? "" : UEProperty(f[1]).GetTypeName();
}

// Resolve the n-th live pair's slot base + key/value props + layout in one shot.
UEValue UEMapRef::KeyAt(int32 n) const
{
    if (!Base || !Prop || n < 0) return {};
    std::vector<void*> f = ScanSubFields(static_cast<uint8*>(Prop.GetAddress()), 2);
    if (f.size() < 2) return {};
    UEProperty kp(f[0]), vp(f[1]);
    const ContainerLayout lay = MapLayoutFor(kp, vp);

    void* data = ReadPtr(Base, 0x00);
    if (!data) return {};
    int32 slot = SparseNthLive(Base, n);
    if (slot < 0) return {};
    uint8* pair = static_cast<uint8*>(data) + static_cast<size_t>(slot) * lay.stride;
    return ReadProperty(kp, pair + lay.keyOff);
}

UEValue UEMapRef::ValueAt(int32 n) const
{
    if (!Base || !Prop || n < 0) return {};
    std::vector<void*> f = ScanSubFields(static_cast<uint8*>(Prop.GetAddress()), 2);
    if (f.size() < 2) return {};
    UEProperty kp(f[0]), vp(f[1]);
    const ContainerLayout lay = MapLayoutFor(kp, vp);

    void* data = ReadPtr(Base, 0x00);
    if (!data) return {};
    int32 slot = SparseNthLive(Base, n);
    if (slot < 0) return {};
    uint8* pair = static_cast<uint8*>(data) + static_cast<size_t>(slot) * lay.stride;
    return ReadProperty(vp, pair + lay.valOff);
}

// ============================ UEObject ========================================

void* UEObject::GetVft() const { return ReadPtr(Object, Off::UObject::Vft); }

EObjectFlags UEObject::GetFlags() const
{
    return static_cast<EObjectFlags>(ReadAt<int32>(Object, Off::UObject::Flags));
}

int32 UEObject::GetIndex() const { return ReadAt<int32>(Object, Off::UObject::Index); }

UEClass UEObject::GetClass() const { return UEClass(ReadPtr(Object, Off::UObject::Class)); }

int32 UEObject::GetNameIndex() const { return ReadAt<int32>(Object, Off::UObject::Name); }
int32 UEObject::GetNameNumber() const { return ReadAt<int32>(Object, Off::UObject::Name + 4); }

UEObject UEObject::GetOuter() const { return UEObject(ReadPtr(Object, Off::UObject::Outer)); }

std::string UEObject::GetName() const
{
    if (!Object) return "";
    return NameArray::GetFullName(GetNameIndex(), GetNameNumber());
}

std::string UEObject::GetClassName() const
{
    UEClass c = GetClass();
    return c ? c.GetName() : "";
}

std::string UEObject::GetPathName() const
{
    if (!Object) return "";

    std::string path = GetName();
    UEObject outer = GetOuter();
    int guard = 0;
    while (outer && guard++ < 32)
    {
        path = outer.GetName() + "." + path;
        outer = outer.GetOuter();
    }
    return path;
}

std::string UEObject::GetFullName() const
{
    if (!Object) return "";
    return GetClassName() + " " + GetPathName();
}

bool UEObject::IsValid() const
{
    return Object && SafeMemory::IsReadable(reinterpret_cast<uintptr_t>(Object)) && GetVft();
}

bool UEObject::IsA(UEClass cls) const
{
    if (!cls) return false;
    UEStruct c = GetClass();
    int guard = 0;
    while (c && guard++ < 64)
    {
        if (c.GetAddress() == cls.GetAddress())
            return true;
        c = c.GetSuper();
    }
    return false;
}

bool UEObject::IsA(EClassCastFlags flags) const
{
    UEClass c = GetClass();
    return c && (c.GetCastFlags() & flags);
}

bool UEObject::IsA(const std::string& className) const
{
    UEStruct c = GetClass();
    int guard = 0;
    while (c && guard++ < 64)
    {
        if (UENames::EqualsCI(c.GetName(), className))
            return true;
        c = c.GetSuper();
    }
    return false;
}

UEProperty UEObject::FindProp(const std::string& name) const
{
    UEClass c = GetClass();
    return c ? c.FindMember(name) : UEProperty();
}

int32 UEObject::GetPropOffset(const std::string& name) const
{
    UEProperty p = FindProp(name);
    return p ? p.GetOffset() : -1;
}

bool UEObject::GetBool(const std::string& name) const
{
    UEProperty p = FindProp(name);
    if (!p || !Object) return false;

    const int32 base = p.GetOffset();
    if (p.IsType("BoolProperty"))
    {
        uint8 byte = ReadAt<uint8>(Object, base + p.GetBoolByteOffset());
        return (byte & p.GetBoolFieldMask()) != 0;
    }
    return ReadAt<uint8>(Object, base) != 0;
}

bool UEObject::SetBool(const std::string& name, bool value)
{
    UEProperty p = FindProp(name);
    if (!p || !Object) return false;

    const int32 base = p.GetOffset();
    if (p.IsType("BoolProperty"))
    {
        uint8* byte = Object + base + p.GetBoolByteOffset();
        const uint8 mask = p.GetBoolFieldMask();
        *byte = value ? (*byte | mask) : (*byte & ~mask);
        return true;
    }
    *(Object + base) = value ? 1 : 0;
    return true;
}

UEObject UEObject::GetObjectProp(const std::string& name) const
{
    const int32 off = GetPropOffset(name);
    if (off < 0) return UEObject();
    return UEObject(ReadPtr(Object, off));
}

std::string UEObject::GetStringProp(const std::string& name) const
{
    const int32 off = GetPropOffset(name);
    if (off < 0 || !Object) return "";

    // FString == TArray<wchar_t>: { wchar_t* Data; int32 Num; int32 Max; }
    void* data = ReadPtr(Object, off);
    int32 num  = ReadAt<int32>(Object, off + sizeof(void*));
    if (!data || num <= 0 || num > 0x100000)
        return "";

    wchar_t buf[1024] = {};
    int n = num < 1023 ? num : 1023;
    for (int i = 0; i < n; ++i)
    {
        if (!SafeMemory::Read<wchar_t>(reinterpret_cast<uintptr_t>(data) + i * sizeof(wchar_t), &buf[i]))
            break;
    }

    std::string out;
    for (int i = 0; i < n && buf[i]; ++i)
        out += (buf[i] >= 0x20 && buf[i] <= 0x7E) ? static_cast<char>(buf[i]) : '?';
    return out;
}

UEFunction UEObject::FindFunction(const std::string& name) const
{
    UEClass c = GetClass();
    return c ? c.GetFunction(name) : UEFunction();
}

void UEObject::ProcessEvent(UEFunction func, void* params)
{
    void* callable = Off::InSDK::ProcessEvent::Callable
                         ? Off::InSDK::ProcessEvent::Callable
                         : Off::InSDK::ProcessEvent::FuncPtr;
    if (!callable || !Object || !func)
        return;

    using PEFn = void(*)(void*, void*, void*);
    reinterpret_cast<PEFn>(callable)(Object, func.GetAddress(), params);
}

bool UEObject::CallFunction(const std::string& name, void* params)
{
    UEFunction fn = FindFunction(name);
    if (!fn) return false;
    ProcessEvent(fn, params);
    return true;
}

UEValue UEObject::GetValue(const std::string& name) const
{
    return ReadProperty(FindProp(name), Object);
}

bool UEObject::SetValue(const std::string& name, const UEValue& value)
{
    return WriteProperty(FindProp(name), Object, value);
}

UEValue UEObject::CallByName(const std::string& funcName,
                            const std::vector<UEValue>& args, bool* ok)
{
    if (ok) *ok = false;

    UEFunction fn = FindFunction(funcName);
    if (!fn || !Object)
        return {};

    // Frame size = the function's total property size (params + locals + return).
    int32 frameSize = fn.GetPropertiesSize();
    if (frameSize <= 0) frameSize = fn.GetParmsSize();
    if (frameSize <= 0 || frameSize > 0x10000)
        return {};

    std::vector<uint8> frame(static_cast<size_t>(frameSize), 0);
    uint8* base = frame.data();

    // Fill inputs positionally, in declaration order, skipping the return value.
    // String/name args allocate transient buffers we free after the call.
    std::vector<void*> allocs;
    const std::vector<UEProperty> params = fn.GetParams();
    size_t argi = 0;
    for (const UEProperty& p : params)
    {
        if (p.GetPropertyFlags() & EPropertyFlags::ReturnParm)
            continue;
        if (argi >= args.size())
            break;
        WriteProperty(p, base, args[argi++], &allocs);
    }

    ProcessEvent(fn, base);

    UEValue result;
    UEProperty ret = fn.GetReturnProperty();
    if (ret)
        result = ReadProperty(ret, base);

    // Materialize an FString return before the frame (and its buffer) is freed.
    if (result.Kind == UEValueKind::String)
        result = UEValue::MakeString(result.Str);
    // A struct/array/map return would point into `frame`, about to be freed.
    if (result.Kind == UEValueKind::Struct || result.Kind == UEValueKind::Array ||
        result.Kind == UEValueKind::Map    || result.Kind == UEValueKind::Set)
        result = {};

    for (void* pmem : allocs)
        delete[] reinterpret_cast<wchar_t*>(pmem);

    if (ok) *ok = true;
    return result;
}

// ============================ UEField ========================================

UEField UEField::GetNext() const { return UEField(ReadPtr(Object, Off::UField::Next)); }

// ============================ UEProperty =====================================

std::string UEProperty::GetName() const
{
    if (!Prop) return "";
    const int32 nameOff = Off::InSDK::bUseFProperty ? Off::FField::Name : Off::UObject::Name;
    int32 idx = ReadAt<int32>(Prop, nameOff);
    int32 num = ReadAt<int32>(Prop, nameOff + 4);
    return NameArray::GetFullName(idx, num);
}

std::string UEProperty::GetTypeName() const
{
    if (!Prop) return "";

    if (Off::InSDK::bUseFProperty)
    {
        // FField::Class -> FFieldClass, whose FName sits at offset 0.
        void* ffieldClass = ReadPtr(Prop, Off::FField::Class);
        if (!ffieldClass) return "";
        int32 idx = ReadAt<int32>(static_cast<uint8*>(ffieldClass), 0);
        return NameArray::GetNameByIndex(idx);
    }

    // UProperty: it's a UObject; its class name is the property type.
    return UEObject(Prop).GetClassName();
}

int32 UEProperty::GetOffset() const       { return ReadAt<int32>(Prop, Off::Property::Offset); }
int32 UEProperty::GetElementSize() const  { return ReadAt<int32>(Prop, Off::Property::ElementSize); }
int32 UEProperty::GetArrayDim() const     { return ReadAt<int32>(Prop, Off::Property::ArrayDim); }

EPropertyFlags UEProperty::GetPropertyFlags() const
{
    return static_cast<EPropertyFlags>(ReadAt<uint64>(Prop, Off::Property::PropertyFlags));
}

bool UEProperty::IsType(const std::string& typeName) const
{
    return GetTypeName() == typeName;
}

uint8 UEProperty::GetBoolFieldMask() const { return ReadAt<uint8>(Prop, Off::FBoolProperty::Base + 3); }
int32 UEProperty::GetBoolByteOffset() const { return ReadAt<uint8>(Prop, Off::FBoolProperty::Base + 1); }

UEProperty UEProperty::GetNext() const
{
    const int32 nextOff = Off::InSDK::bUseFProperty ? Off::FField::Next : Off::UField::Next;
    return UEProperty(ReadPtr(Prop, nextOff));
}

UEStruct UEProperty::GetStructType() const
{
    if (!Prop) return UEStruct();
    void* ss = ScanSubPointer(Prop, [](void* q) { return LooksLikeScriptStruct(q); });
    return UEStruct(ss);
}

UEClass UEProperty::GetObjectClass() const
{
    if (!Prop) return UEClass();
    void* c = ScanSubPointer(Prop, [](void* q) { return LooksLikeUObjectOfClass(q, "Class"); });
    return UEClass(c);
}

UEEnum UEProperty::GetEnum() const
{
    if (!Prop) return UEEnum();
    void* e = ScanSubPointer(Prop, [](void* q) { return LooksLikeUObjectOfClass(q, "Enum"); });
    return UEEnum(e);
}

UEProperty UEProperty::GetInnerProperty() const
{
    if (!Prop) return UEProperty();
    std::vector<void*> f = ScanSubFields(Prop, 1);
    return f.empty() ? UEProperty() : UEProperty(f[0]);
}

UEProperty UEProperty::GetKeyProperty() const
{
    if (!Prop) return UEProperty();
    std::vector<void*> f = ScanSubFields(Prop, 2);
    return f.size() < 1 ? UEProperty() : UEProperty(f[0]);
}

UEProperty UEProperty::GetValueProperty() const
{
    if (!Prop) return UEProperty();
    std::vector<void*> f = ScanSubFields(Prop, 2);
    return f.size() < 2 ? UEProperty() : UEProperty(f[1]);
}

// ============================ UEStruct =======================================

UEStruct UEStruct::GetSuper() const { return UEStruct(ReadPtr(Object, Off::UStruct::SuperStruct)); }
void* UEStruct::GetChildren() const { return ReadPtr(Object, Off::UStruct::Children); }
void* UEStruct::GetChildProperties() const { return ReadPtr(Object, Off::UStruct::ChildProperties); }
int32 UEStruct::GetPropertiesSize() const { return ReadAt<int32>(Object, Off::UStruct::Size); }

std::vector<UEProperty> UEStruct::GetProperties() const
{
    std::vector<UEProperty> out;
    if (!Object) return out;

    int guard = 0;
    if (Off::InSDK::bUseFProperty)
    {
        UEProperty p(GetChildProperties());
        while (p && guard++ < 8192)
        {
            out.push_back(p);
            p = p.GetNext();
        }
    }
    else
    {
        // UProperties live in the Children chain, interleaved with functions.
        UEField f(GetChildren());
        while (f && guard++ < 8192)
        {
            UEProperty p(f.GetAddress());
            const std::string t = p.GetTypeName();
            if (t.size() >= 8 && t.compare(t.size() - 8, 8, "Property") == 0)
                out.push_back(p);
            f = f.GetNext();
        }
    }
    return out;
}

std::vector<UEFunction> UEStruct::GetFunctions() const
{
    std::vector<UEFunction> out;
    if (!Object) return out;

    UEField f(GetChildren());
    int guard = 0;
    while (f && guard++ < 8192)
    {
        if (UEObject(f.GetAddress()).GetClassName() == "Function")
            out.push_back(UEFunction(f.GetAddress()));
        f = f.GetNext();
    }
    return out;
}

std::vector<UEProperty> UEStruct::GetAllProperties() const
{
    std::vector<UEProperty> out;
    int guard = 0;
    for (UEStruct s = *this; s && guard++ < 64; s = s.GetSuper())
        for (const UEProperty& p : s.GetProperties())
            out.push_back(p);
    return out;
}

std::vector<UEFunction> UEStruct::GetAllFunctions() const
{
    std::vector<UEFunction> out;
    int guard = 0;
    for (UEStruct s = *this; s && guard++ < 64; s = s.GetSuper())
        for (const UEFunction& fn : s.GetFunctions())
            out.push_back(fn);
    return out;
}

UEProperty UEStruct::FindMember(const std::string& name) const
{
    UEStruct s = *this;
    int guard = 0;
    while (s && guard++ < 64)
    {
        for (const UEProperty& p : s.GetProperties())
        {
            if (UENames::EqualsCI(p.GetName(), name))
                return p;
        }
        s = s.GetSuper();
    }
    return UEProperty();
}

UEFunction UEStruct::FindFunctionInHierarchy(const std::string& name) const
{
    UEStruct s = *this;
    int guard = 0;
    while (s && guard++ < 64)
    {
        for (const UEFunction& fn : s.GetFunctions())
        {
            if (UENames::EqualsCI(fn.GetName(), name))
                return fn;
        }
        s = s.GetSuper();
    }
    return UEFunction();
}

bool UEStruct::IsChildOf(UEStruct parent) const
{
    if (!parent) return false;
    UEStruct s = *this;
    int guard = 0;
    while (s && guard++ < 64)
    {
        if (s.GetAddress() == parent.GetAddress())
            return true;
        s = s.GetSuper();
    }
    return false;
}

// ============================ UEFunction =====================================

EFunctionFlags UEFunction::GetFunctionFlags() const
{
    return static_cast<EFunctionFlags>(ReadAt<uint32>(Object, Off::UFunction::FunctionFlags));
}

bool UEFunction::HasFlags(EFunctionFlags flags) const { return GetFunctionFlags() & flags; }
void* UEFunction::GetExecFunction() const { return ReadPtr(Object, Off::UFunction::ExecFunction); }
int32 UEFunction::GetParmsSize() const { return ReadAt<uint16>(Object, Off::UFunction::ParmsSize); }
uint8 UEFunction::GetNumParms() const { return ReadAt<uint8>(Object, Off::UFunction::NumParms); }

std::vector<UEProperty> UEFunction::GetParams() const
{
    // A UFunction's properties are exactly its parameters (incl. the return).
    return GetProperties();
}

UEProperty UEFunction::GetReturnProperty() const
{
    for (const UEProperty& p : GetProperties())
    {
        if (p.GetPropertyFlags() & EPropertyFlags::ReturnParm)
            return p;
    }
    return UEProperty();
}

// ============================ UEParams =======================================

UEProperty UEParams::Find(const std::string& name) const
{
    for (const UEProperty& p : Func.GetParams())
    {
        if (p.GetName() == name)
            return p;
    }
    return UEProperty();
}

int32 UEParams::OffsetOf(const std::string& name) const
{
    UEProperty p = Find(name);
    return p ? p.GetOffset() : -1;
}

bool UEParams::GetBool(const std::string& name) const
{
    UEProperty p = Find(name);
    if (!p || !Base) return false;

    const int32 base = p.GetOffset();
    if (p.IsType("BoolProperty"))
    {
        uint8 byte = ReadAt<uint8>(Base, base + p.GetBoolByteOffset());
        return (byte & p.GetBoolFieldMask()) != 0;
    }
    return ReadAt<uint8>(Base, base) != 0;
}

bool UEParams::SetBool(const std::string& name, bool value)
{
    UEProperty p = Find(name);
    if (!p || !Base) return false;

    const int32 base = p.GetOffset();
    if (p.IsType("BoolProperty"))
    {
        uint8* byte = Base + base + p.GetBoolByteOffset();
        const uint8 mask = p.GetBoolFieldMask();
        *byte = value ? (*byte | mask) : (*byte & ~mask);
        return true;
    }
    *(Base + base) = value ? 1 : 0;
    return true;
}

UEObject UEParams::GetObject(const std::string& name) const
{
    const int32 off = OffsetOf(name);
    if (off < 0 || !Base) return UEObject();
    return UEObject(ReadPtr(Base, off));
}

std::string UEParams::GetString(const std::string& name) const
{
    const int32 off = OffsetOf(name);
    if (off < 0 || !Base) return "";

    // FString == TArray<wchar_t>: { wchar_t* Data; int32 Num; int32 Max; }
    void* data = ReadPtr(Base, off);
    int32 num  = ReadAt<int32>(Base, off + sizeof(void*));
    if (!data || num <= 0 || num > 0x100000)
        return "";

    std::string out;
    const int n = num < 1023 ? num : 1023;
    for (int i = 0; i < n; ++i)
    {
        wchar_t c = 0;
        if (!SafeMemory::Read<wchar_t>(reinterpret_cast<uintptr_t>(data) + i * sizeof(wchar_t), &c) || !c)
            break;
        out += (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
    }
    return out;
}

UEValue UEParams::GetValue(const std::string& name) const
{
    return ReadProperty(Find(name), Base);
}

bool UEParams::SetValue(const std::string& name, const UEValue& value)
{
    return WriteProperty(Find(name), Base, value);
}

// ============================ UEClass ========================================

EClassCastFlags UEClass::GetCastFlags() const
{
    return static_cast<EClassCastFlags>(ReadAt<uint64>(Object, Off::UClass::CastFlags));
}

bool UEClass::IsType(EClassCastFlags flags) const { return GetCastFlags() & flags; }

UEObject UEClass::GetDefaultObject() const
{
    return UEObject(ReadPtr(Object, Off::UClass::ClassDefaultObject));
}

UEFunction UEClass::GetFunction(const std::string& name) const
{
    return FindFunctionInHierarchy(name);
}

// ============================ UEEnum =========================================

std::vector<std::pair<std::string, int64>> UEEnum::GetNames() const
{
    std::vector<std::pair<std::string, int64>> out;
    if (!Object) return out;

    const int off = ResolveEnumNamesOffset(Object);
    if (off < 0) return out;

    void* data = ReadPtr(Object, off);
    int32 num  = ReadAt<int32>(Object, off + sizeof(void*));
    if (!data || num < 1 || num > 0x4000) return out;

    constexpr int stride = 0x10; // sizeof(TPair<FName,int64>)
    for (int32 i = 0; i < num; ++i)
    {
        const uintptr_t entry = reinterpret_cast<uintptr_t>(data) + static_cast<size_t>(i) * stride;
        int32 idx = 0;
        SafeMemory::Read<int32>(entry, &idx);
        int64 value = 0;
        SafeMemory::Read<int64>(entry + 8, &value);
        std::string name = NameArray::GetNameByIndex(idx);
        if (name.empty()) continue;
        out.emplace_back(std::move(name), value);
    }
    return out;
}

std::vector<std::pair<std::string, int64>> UEEnum::GetShortNames() const
{
    std::vector<std::pair<std::string, int64>> out = GetNames();
    for (auto& p : out)
    {
        const size_t pos = p.first.rfind("::");
        if (pos != std::string::npos)
            p.first = p.first.substr(pos + 2);
    }
    return out;
}

std::string UEEnum::GetNameByValue(int64 value) const
{
    for (const auto& p : GetNames())
        if (p.second == value)
        {
            const size_t pos = p.first.rfind("::");
            return pos == std::string::npos ? p.first : p.first.substr(pos + 2);
        }
    return "";
}

int64 UEEnum::GetValueByName(const std::string& name) const
{
    for (const auto& p : GetNames())
    {
        if (p.first == name) return p.second;
        const size_t pos = p.first.rfind("::");
        if (pos != std::string::npos && p.first.substr(pos + 2) == name)
            return p.second;
    }
    return -1;
}
