#include "OffsetFinder.h"
#include "Offsets.h"
#include "Core/SafeMemory.h"
#include "Core/MemoryScanner.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Reflection/CoreUObject.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <vector>

namespace
{
    // Hard cap on how many GObjects entries any single detection pass will walk.
    // Core reflection objects (UClass "Class"/"Object", core properties) are
    // registered at engine startup and live very early in the array, so this is
    // plenty to detect layout while bounding cost on games with 100k+ objects.
    constexpr int32 kScanCap = 200000;

    void Log(const char* fmt, ...)
    {
        if (!OffsetFinder::Verbose) return;
        va_list args;
        va_start(args, fmt);
        std::printf("[UEHook][detect] ");
        std::vprintf(fmt, args);
        std::printf("\n");
        va_end(args);
    }

    int32 RawIndex(void* obj)
    {
        int32 v = -1;
        if (obj)
            SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(obj) + 0x0C, &v);
        return v;
    }

    void* RawPtr(void* base, int32 off)
    {
        uintptr_t v = 0;
        if (base)
            SafeMemory::Read<uintptr_t>(reinterpret_cast<uintptr_t>(base) + off, &v);
        return reinterpret_cast<void*>(v);
    }

    std::vector<void*> SampleObjects(int max)
    {
        std::vector<void*> out;
        const int32 count = ObjectArray::Num();
        const int32 limit = count < kScanCap ? count : kScanCap;
        for (int32 i = 0; i < limit && (int)out.size() < max; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (o && SafeMemory::IsReadable(reinterpret_cast<uintptr_t>(o)))
                out.push_back(o);
        }
        return out;
    }

    // Single scan: collect UClass objects (class name == "Class") for reuse by
    // every offset-scoring detector, so we never rescan the whole array per
    // candidate offset.
    std::vector<void*> CollectClasses(int max)
    {
        std::vector<void*> out;
        const int32 count = ObjectArray::Num();
        const int32 limit = count < kScanCap ? count : kScanCap;
        for (int32 i = 0; i < limit && (int)out.size() < max; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            if (UEObject(o).GetClassName() == "Class")
                out.push_back(o);
        }
        return out;
    }
}

static bool DetectItemSize(uintptr_t gobjects, bool chunked)
{
    for (int32 candidate : { 0x18, 0x10, 0x20, 0x1C })
    {
        Off::FUObjectItem::Size = candidate;
        ObjectArray::Init(gobjects, chunked);

        int hits = 0, tested = 0;
        const int32 count = ObjectArray::Num();
        for (int32 i = 1; i < count && tested < 12; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            ++tested;
            if (RawIndex(o) == i) ++hits;
        }
        if (tested >= 4 && hits >= tested - 1)
        {
            Log("item size = 0x%X (objects: %d)", candidate, ObjectArray::Num());
            return true;
        }
    }
    Off::FUObjectItem::Size = 0x18;
    ObjectArray::Init(gobjects, chunked);
    Log("item size detection failed, using default 0x18 (objects: %d)", ObjectArray::Num());
    return false;
}

static bool DetectClassOffset()
{
    auto samples = SampleObjects(50);
    for (int32 candidate : { 0x10, 0x18, 0x14 })
    {
        int converged = 0;
        for (void* o : samples)
        {
            void* c1 = RawPtr(o, candidate);
            if (!SafeMemory::IsReadable(reinterpret_cast<uintptr_t>(c1))) continue;
            void* c2 = RawPtr(c1, candidate);
            void* c3 = RawPtr(c2, candidate);
            if (c2 && c2 == c3) ++converged;
        }
        if (converged >= (int)samples.size() / 2 && converged > 0)
        {
            Off::UObject::Class = candidate;
            Log("class offset = 0x%X", candidate);
            return true;
        }
    }
    Log("class offset detection failed, using default 0x%X", Off::UObject::Class);
    return false;
}

static void HexDump(const char* label, uintptr_t addr, int n)
{
    if (!OffsetFinder::Verbose) return;
    std::printf("[UEHook][detect]   %-18s 0x%p:", label, (void*)addr);
    for (int i = 0; i < n; ++i)
    {
        uint8 b = 0;
        SafeMemory::Read<uint8>(addr + i, &b);
        std::printf(" %02X", b);
    }
    std::printf("\n");
}

// Enumerate the module's writable / initialized-data sections (.data, .bss,
// .rdata). GNames lives in one of these (either the pool itself, or a pointer to
// a heap-allocated pool), never in .text -- so we skip the huge code section.
struct SecRange { uintptr_t start; uintptr_t end; };

static std::vector<SecRange> DataSections(uintptr_t base)
{
    std::vector<SecRange> out;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;

    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        const DWORD ch = sec->Characteristics;
        const bool writable = (ch & IMAGE_SCN_MEM_WRITE) != 0;
        const bool initData = (ch & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0;
        const bool code     = (ch & IMAGE_SCN_CNT_CODE) != 0;
        if (code) continue;                 // skip .text
        if (!writable && !initData) continue;

        uintptr_t s = base + sec->VirtualAddress;
        uintptr_t e = s + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        out.push_back({ s, e });
    }
    return out;
}

// Configure NameArray for `base` (as pool or legacy), then find the UObject
// name offset by requiring the metaclass FName to decode to "Class". Both
// InitPool and InitLegacy self-validate the base against known name strings.
static bool ConfigureFor(uintptr_t base, bool pool, void* meta, int32& outNameOff)
{
    if (pool ? !NameArray::InitPool(base) : !NameArray::InitLegacy(base))
        return false;

    for (int32 no : { 0x18, 0x14, 0x1C, 0x20, 0x10 })
    {
        int32 idx = 0;
        if (!SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(meta) + no, &idx)) continue;
        if (idx <= 0 || idx > 0x10000000) continue;
        if (NameArray::GetNameByIndex(idx) == "Class")
        {
            outNameOff = no;
            return true;
        }
    }
    return false;
}

// Scan the module's data sections for the real GNames: each candidate base is
// validated by InitPool/InitLegacy (which look for "None"/"/Script/CoreUObject")
// and by the metaclass decoding to "Class".
static bool ScanForGNames(void* meta, int32& outNameOff)
{
    MemoryScanner scanner;
    const uintptr_t modBase = scanner.GetModuleBase();
    if (!modBase) return false;

    std::vector<SecRange> ranges = DataSections(modBase);
    if (ranges.empty()) return false;

    Log("scanning %d data sections for GNames...", (int)ranges.size());

    for (const SecRange& r : ranges)
    {
        for (uintptr_t a = r.start; a + 8 <= r.end; a += 8)
        {
            uintptr_t val = 0;
            SafeMemory::Read<uintptr_t>(a, &val);

            for (uintptr_t P : { val, a })
            {
                if (!SafeMemory::IsReasonable(P)) continue;
                if (ConfigureFor(P, /*pool*/true, meta, outNameOff))
                {
                    Log("GNames FOUND (pool) base=0x%p", (void*)P);
                    return true;
                }
                if (ConfigureFor(P, /*pool*/false, meta, outNameOff))
                {
                    Log("GNames FOUND (legacy) base=0x%p", (void*)P);
                    return true;
                }
            }
        }
    }
    return false;
}

// Detects the FName base + layout + UObject::Name offset. Tries the pattern-
// found base first, then a full validated module scan. On success, refines
// FNameBlockOffsetBits.
static bool DetectNames()
{
    void* sample = nullptr;
    for (void* o : SampleObjects(10)) { sample = o; break; }
    if (!sample) return false;

    void* meta = RawPtr(sample, Off::UObject::Class);
    void* next = RawPtr(meta, Off::UObject::Class);
    if (next) meta = next;

    const uintptr_t gnamesRaw = NameArray::GetBase();
    int32 nameOff = 0x18;

    // Fast path: pattern base, and one dereference of it.
    uintptr_t fastBases[2] = { gnamesRaw, 0 };
    int numFast = 1;
    uintptr_t deref = 0;
    if (SafeMemory::Read<uintptr_t>(gnamesRaw, &deref) && SafeMemory::IsReasonable(deref))
    {
        fastBases[1] = deref;
        numFast = 2;
    }

    bool found = false;
    for (int i = 0; i < numFast && !found; ++i)
    {
        if (ConfigureFor(fastBases[i], true, meta, nameOff) ||
            ConfigureFor(fastBases[i], false, meta, nameOff))
        {
            Log("names: pattern base 0x%p (%s)", (void*)NameArray::GetBase(),
                NameArray::UsingPool() ? "pool" : "legacy");
            found = true;
        }
    }

    if (!found)
        found = ScanForGNames(meta, nameOff);

    if (!found)
    {
        NameArray::SetBase(gnamesRaw);
        int32 idx = 0;
        SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(meta) + 0x18, &idx);
        Log("name detection FAILED -- diagnostics:");
        Log("metaclass=0x%p  idx@0x18=%d", meta, idx);
        HexDump("metaclass[0x40]", reinterpret_cast<uintptr_t>(meta), 0x40);
        HexDump("GNames[0x40]", gnamesRaw, 0x40);
        if (numFast == 2) HexDump("*GNames[0x40]", deref, 0x40);
        return false;
    }

    Off::UObject::Name  = nameOff;
    Off::UObject::Outer = nameOff + 8;

    if (NameArray::UsingPool())
    {
        NameArray::DetermineBlockBits();
        Log("names: name offset=0x%X, block bits refined", nameOff);
    }
    return true;
}

static bool DetectSuperOffset(const std::vector<void*>& classes)
{
    void* classClass = nullptr;
    for (void* c : classes)
        if (UEObject(c).GetName() == "Class") { classClass = c; break; }
    if (!classClass) return false;

    for (int32 candidate : { 0x40, 0x48, 0x30, 0x38, 0x44 })
    {
        void* cur = classClass;
        bool reachedObject = false;
        for (int hop = 0; hop < 12 && cur; ++hop)
        {
            if (UEObject(cur).GetName() == "Object") { reachedObject = true; break; }
            cur = RawPtr(cur, candidate);
            if (!SafeMemory::IsReadable(reinterpret_cast<uintptr_t>(cur))) { cur = nullptr; break; }
        }
        if (reachedObject)
        {
            Off::UStruct::SuperStruct = candidate;
            Log("super offset = 0x%X", candidate);
            return true;
        }
    }
    Log("super offset detection failed, using default 0x%X", Off::UStruct::SuperStruct);
    return false;
}

static bool DetectChildrenOffset(const std::vector<void*>& classes)
{
    auto scoreFor = [&](int32 candidate) -> int
    {
        int score = 0;
        for (void* o : classes)
        {
            void* child = RawPtr(o, candidate);
            for (int hop = 0; hop < 64 && child; ++hop)
            {
                std::string cn = UEObject(child).GetClassName();
                if (cn == "Function" ||
                    (cn.size() >= 8 && cn.compare(cn.size() - 8, 8, "Property") == 0))
                    ++score;
                else if (!cn.empty() && cn != "Enum" && cn != "ScriptStruct")
                    break;
                child = RawPtr(child, Off::UField::Next);
            }
        }
        return score;
    };

    int32 best = -1; int bestScore = 0;
    for (int32 candidate : { 0x48, 0x50, 0x40, 0x38, 0x30 })
    {
        int s = scoreFor(candidate);
        if (s > bestScore) { bestScore = s; best = candidate; }
    }
    if (best >= 0 && bestScore > 4)
    {
        Off::UStruct::Children = best;
        Log("children offset = 0x%X (score %d)", best, bestScore);
        return true;
    }
    Log("children offset detection failed, using default 0x%X", Off::UStruct::Children);
    return false;
}

static bool DetectPropertyChain(const std::vector<void*>& classes)
{
    auto goodPropNames = [&](int32 childPropsOff) -> int
    {
        int score = 0;
        for (void* o : classes)
        {
            void* field = RawPtr(o, childPropsOff);
            for (int hop = 0; hop < 64 && field; ++hop)
            {
                void* ffieldClass = RawPtr(field, Off::FField::Class);
                if (!ffieldClass) break;
                int32 idx = 0;
                SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(ffieldClass), &idx);
                std::string tn = NameArray::GetNameByIndex(idx);
                if (tn.size() >= 8 && tn.compare(tn.size() - 8, 8, "Property") == 0)
                    ++score;
                else
                    break;
                field = RawPtr(field, Off::FField::Next);
            }
        }
        return score;
    };

    int32 best = -1; int bestScore = 0;
    for (int32 candidate : { 0x50, 0x48, 0x58, 0x40 })
    {
        int s = goodPropNames(candidate);
        if (s > bestScore) { bestScore = s; best = candidate; }
    }

    if (best >= 0 && bestScore > 4)
    {
        Off::UStruct::ChildProperties = best;
        Off::InSDK::bUseFProperty = true;
        Log("child-properties offset = 0x%X (FProperty world, score %d)", best, bestScore);
        return true;
    }

    // Older UE: properties are UProperty (UField-based). Member offsets differ
    // from the FProperty defaults, so switch to the UProperty x64 layout.
    Off::InSDK::bUseFProperty     = false;
    Off::Property::ArrayDim       = 0x30;
    Off::Property::ElementSize    = 0x34;
    Off::Property::PropertyFlags  = 0x38;
    Off::Property::Offset         = 0x44;
    Log("no FProperty chain found -> UProperty world (older UE); using UProperty member offsets");
    return true;
}

namespace
{
    // Scan [start, start+range) for a byte pattern. Negative entries are wildcards.
    // `start` must point at valid, readable code; `range` is clamped by the caller.
    bool FindPatternInRange(const std::vector<int>& pat, const uint8_t* start, size_t range)
    {
        if (pat.empty() || range < pat.size())
            return false;
        const size_t last = range - pat.size();
        for (size_t i = 0; i <= last; ++i)
        {
            bool ok = true;
            for (size_t j = 0; j < pat.size(); ++j)
            {
                if (pat[j] < 0) continue;
                if (start[i + j] != static_cast<uint8_t>(pat[j])) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }
}

// UE 5.1.1 shrank FFieldVariant from { void*, bool } to { void* }, removing 8
// bytes from FField::Owner. That pushes every FField/FProperty member after Owner
// (Next, Name, Flags, and all the Property::* offsets) down by 0x08. We detect
// it by checking, on real properties, whether the "next" pointer lives at the
// compact offset (0x18) or the standard one (0x20). Only meaningful in the
// FProperty world.
static void DetectFFieldLayout(const std::vector<void*>& classes)
{
    if (!Off::InSDK::bUseFProperty)
        return;

    auto looksLikeField = [](void* field) -> bool
    {
        if (!field || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(field)))
            return false;
        void* fclass = RawPtr(field, Off::FField::Class); // 0x08, before the shift
        if (!fclass)
            return false;
        int32 idx = 0;
        SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(fclass), &idx);
        std::string tn = NameArray::GetNameByIndex(idx);
        return tn.size() >= 8 && tn.compare(tn.size() - 8, 8, "Property") == 0;
    };

    int compactVotes = 0, standardVotes = 0;
    for (void* c : classes)
    {
        void* first = RawPtr(c, Off::UStruct::ChildProperties); // 0x50
        if (!looksLikeField(first))
            continue;

        // In the compact layout Next sits at 0x18; in the standard one at 0x20.
        const bool compact  = looksLikeField(RawPtr(first, 0x18));
        const bool standard = looksLikeField(RawPtr(first, 0x20));
        if (compact && !standard) ++compactVotes;
        else if (standard && !compact) ++standardVotes;
        // both/neither -> single-property class or ambiguous; ignore
    }

    if (compactVotes > standardVotes && compactVotes > 2)
    {
        Off::FField::Next          -= 0x08; // 0x20 -> 0x18
        Off::FField::Name          -= 0x08; // 0x28 -> 0x20
        Off::FField::Flags         -= 0x08; // 0x30 -> 0x28
        Off::Property::ArrayDim     -= 0x08;
        Off::Property::ElementSize  -= 0x08;
        Off::Property::PropertyFlags -= 0x08;
        Off::Property::Offset       -= 0x08; // 0x4C -> 0x44
        Off::FBoolProperty::Base    -= 0x08;
        Log("FField layout: compact FFieldVariant (UE5.1.1+); Name=0x%X Next=0x%X PropOffset=0x%X",
            Off::FField::Name, Off::FField::Next, Off::Property::Offset);
    }
    else
    {
        Log("FField layout: standard; Name=0x%X Next=0x%X PropOffset=0x%X (compact=%d standard=%d)",
            Off::FField::Name, Off::FField::Next, Off::Property::Offset, compactVotes, standardVotes);
    }
}

bool OffsetFinder::DetectProcessEvent()
{
    // Robust, version-independent detection (Dumper-7 approach): ProcessEvent is
    // a virtual function of UObject, so it lives in every object's vtable. Its
    // body is identifiable because it tests the called UFunction's FunctionFlags
    // for FUNC_Native (0x400) and 0x400000 (out-params / net). We walk the vtable
    // of a real object and match those two TEST instructions. This beats a raw
    // module signature scan, which matches thousands of identical prologues.
    MemoryScanner scanner;
    const uintptr_t modBase = scanner.GetModuleBase();
    const uintptr_t modEnd  = modBase + scanner.GetModuleSize();

    // Find any object whose vtable pointer lands inside the main module.
    void** vft = nullptr;
    const int32 count = ObjectArray::Num();
    for (int32 i = 0; i < 64 && i < count; ++i)
    {
        void* o = ObjectArray::GetByIndex(i);
        if (!o) continue;
        uintptr_t v = 0;
        if (SafeMemory::Read<uintptr_t>(reinterpret_cast<uintptr_t>(o), &v) &&
            v >= modBase && v < modEnd)
        {
            vft = reinterpret_cast<void**>(v);
            break;
        }
    }
    if (!vft)
    {
        Log("ProcessEvent: could not find a module-owned object vtable");
        return false;
    }

    const int ff = Off::UFunction::FunctionFlags;
    // F7 /0 id  ->  test dword ptr [reg+FunctionFlags(disp32)], imm32
    const std::vector<int> pat1 = { 0xF7, -1, ff, 0x0, 0x0, 0x0, 0x0, 0x04, 0x0, 0x0 }; // imm 0x400
    const std::vector<int> pat2 = { 0xF7, -1, ff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x40, 0x0 }; // imm 0x400000

    Log("scanning object vtable for ProcessEvent (FunctionFlags=0x%X)...", ff);
    for (int32 i = 0; i < 512; ++i)
    {
        uintptr_t fa = 0;
        if (!SafeMemory::Read<uintptr_t>(reinterpret_cast<uintptr_t>(vft + i), &fa))
            break;
        if (fa < modBase || fa >= modEnd)
            break; // walked past the end of the vtable

        const uint8_t* code = reinterpret_cast<const uint8_t*>(fa);
        const size_t   room = static_cast<size_t>(modEnd - fa);
        const size_t   r1   = room < 0x400 ? room : 0x400;
        const size_t   r2   = room < 0xF00 ? room : 0xF00;

        if (FindPatternInRange(pat1, code, r1) && FindPatternInRange(pat2, code, r2))
        {
            Off::InSDK::ProcessEvent::FuncPtr = reinterpret_cast<void*>(fa);
            Off::InSDK::ProcessEvent::Index   = i;
            Log("ProcessEvent found at vtable[%d] = 0x%p", i, (void*)fa);
            return true;
        }
    }

    Log("ProcessEvent not found via vtable (set cfg.processEventAddr to override)");
    return false;
}

OffsetFinder::Result OffsetFinder::Run(uintptr_t gObjectsAddr, bool bIsChunked)
{
    Result r;
    const DWORD t0 = GetTickCount();

    Log("starting offset detection...");
    r.ItemSize    = DetectItemSize(gObjectsAddr, bIsChunked);
    r.ClassOffset = DetectClassOffset();
    r.NameOffset  = DetectNames();

    Log("collecting class sample...");
    std::vector<void*> classes = CollectClasses(400);
    Log("collected %d classes", (int)classes.size());

    r.SuperOffset    = DetectSuperOffset(classes);
    r.ChildrenOffset = DetectChildrenOffset(classes);
    r.PropChain      = DetectPropertyChain(classes);
    DetectFFieldLayout(classes);
    r.ProcessEvent   = DetectProcessEvent();

    Log("detection complete in %lu ms", GetTickCount() - t0);
    return r;
}
