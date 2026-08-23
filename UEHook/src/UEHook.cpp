#include "UEHook.h"
#include "Engine/Offsets.h"
#include "Engine/OffsetFinder.h"
#include "Core/UEGlobalsDetector.h"
#include "Core/SafeMemory.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include <cstdio>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <string>

namespace
{
    bool         g_initialized = false;
    uintptr_t    g_gWorld = 0;

    void ApplyOverrides(const UEHookConfig& cfg)
    {
        if (cfg.itemSize              >= 0) Off::FUObjectItem::Size     = cfg.itemSize;
        if (cfg.classOffset           >= 0) Off::UObject::Class         = cfg.classOffset;
        if (cfg.nameOffset            >= 0) Off::UObject::Name          = cfg.nameOffset;
        if (cfg.outerOffset           >= 0) Off::UObject::Outer         = cfg.outerOffset;
        if (cfg.superOffset           >= 0) Off::UStruct::SuperStruct   = cfg.superOffset;
        if (cfg.childrenOffset        >= 0) Off::UStruct::Children      = cfg.childrenOffset;
        if (cfg.childPropertiesOffset >= 0) Off::UStruct::ChildProperties = cfg.childPropertiesOffset;
        if (cfg.propOffset            >= 0) Off::Property::Offset       = cfg.propOffset;
        if (cfg.useFProperty          >= 0) Off::InSDK::bUseFProperty   = cfg.useFProperty != 0;

        if (cfg.processEventAddr)           Off::InSDK::ProcessEvent::FuncPtr = cfg.processEventAddr;
    }
}

namespace UE
{
    bool Initialize(const UEHookConfig& cfg)
    {
        if (g_initialized)
            return true;

        UEGlobalsDetector detector;
        if (!detector.FindUEGlobals())
        {
            if (cfg.verbose) std::printf("[UEHook] Failed to locate GObjects/GNames/GWorld.\n");
            return false;
        }

        const uintptr_t gobjects = detector.GetGObjects();
        const uintptr_t gnames   = detector.GetFNames();
        g_gWorld                 = detector.GetGWorld();

        bool chunked = detector.IsChunkedObjectArray();
        if (cfg.isChunked >= 0) chunked = cfg.isChunked != 0;
        Off::InSDK::bIsChunked = chunked;

        bool isPool = true;
        if (cfg.isFNamePool >= 0) isPool = cfg.isFNamePool != 0;
        Off::InSDK::Name::bIsUsingFNamePool = isPool;

        if (!NameArray::Init(gnames, isPool))
        {
            if (cfg.verbose) std::printf("[UEHook] NameArray init failed (index 0 didn't resolve).\n");
            // keep going; some reflection still works, names won't
        }

        OffsetFinder::Verbose = cfg.verbose;
        OffsetFinder::Result det = OffsetFinder::Run(gobjects, chunked);

        // Consumer overrides win over detection.
        ApplyOverrides(cfg);

        // Re-init the object array in case item size changed via override.
        ObjectArray::Init(gobjects, chunked);

        if (cfg.verbose)
        {
            std::printf("[UEHook] Detection: item=%d class=%d name=%d super=%d children=%d prop=%d PE=%d\n",
                        det.ItemSize, det.ClassOffset, det.NameOffset, det.SuperOffset,
                        det.ChildrenOffset, det.PropChain, det.ProcessEvent);
            std::printf("[UEHook] Offsets: Class=0x%X Name=0x%X Super=0x%X Children=0x%X ChildProps=0x%X FProperty=%d\n",
                        Off::UObject::Class, Off::UObject::Name, Off::UStruct::SuperStruct,
                        Off::UStruct::Children, Off::UStruct::ChildProperties, (int)Off::InSDK::bUseFProperty);
            std::printf("[UEHook] Objects: %d  ProcessEvent=0x%p\n", ObjectArray::Num(), Off::InSDK::ProcessEvent::FuncPtr);
        }

        // Manual calls default to the raw ProcessEvent; the hook overrides this.
        if (!Off::InSDK::ProcessEvent::Callable)
            Off::InSDK::ProcessEvent::Callable = Off::InSDK::ProcessEvent::FuncPtr;

        if (cfg.installHook && Off::InSDK::ProcessEvent::FuncPtr)
        {
            if (!Hooking::Install() && cfg.verbose)
                std::printf("[UEHook] ProcessEvent hook failed to install.\n");
        }

        g_initialized = true;
        return true;
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;
        Hooking::Uninstall();
        g_initialized = false;
    }

    bool IsInitialized() { return g_initialized; }

    UEObject GetWorld()
    {
        if (!g_gWorld) return UEObject();
        uintptr_t world = 0;
        SafeMemory::Read<uintptr_t>(g_gWorld, &world);
        return UEObject(reinterpret_cast<void*>(world));
    }

    UEObject GetEngine()
    {
        // First non-default object whose class derives from "Engine".
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.IsA("Engine") && !(obj.GetFlags() & EObjectFlags::ClassDefaultObject))
                return obj;
        }
        return UEObject();
    }

    UEObject FindObject(const std::string& name, const std::string& className)
    {
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (!UENames::EqualsCI(obj.GetName(), name)) continue;
            if (!className.empty() && !obj.IsA(className)) continue;
            return obj;
        }
        return UEObject();
    }

    UEClass FindClass(const std::string& name)
    {
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.GetClassName() == "Class" && UENames::EqualsCI(obj.GetName(), name))
                return UEClass(o);
        }
        return UEClass();
    }

    UEFunction FindFunction(const std::string& name)
    {
        // accepts short name, full path, or "Function <full path>"
        std::string r = name;
        if (r.rfind("Function ", 0) == 0) r = r.substr(9);
        bool isPath = r.find(':') != std::string::npos || r.find('/') != std::string::npos;

        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.GetClassName() != "Function") continue;
            bool match = isPath ? UENames::EqualsCI(obj.GetPathName(), r)
                                : UENames::EqualsCI(obj.GetName(), r);
            if (match) return UEFunction(o);
        }
        return UEFunction();
    }

    std::string ResolveName(int32 comparisonIndex)
    {
        return NameArray::GetNameByIndex(comparisonIndex);
    }

    // A class-default object / archetype -- the `Default__*` template, not a live
    // instance. Detected by object flags (robust) with a name-prefix fallback.
    static bool IsDefaultObject(const UEObject& obj)
    {
        const EObjectFlags f = obj.GetFlags();
        if ((f & EObjectFlags::ClassDefaultObject) || (f & EObjectFlags::ArchetypeObject))
            return true;
        return obj.GetName().rfind("Default__", 0) == 0;
    }

    std::vector<UEObject> GetObjectsOfClass(const std::string& className, bool includeDefaults)
    {
        std::vector<UEObject> out;
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (!obj.IsA(className)) continue;
            if (!includeDefaults && IsDefaultObject(obj)) continue;
            out.push_back(obj);
        }
        return out;
    }

    std::vector<UEClass> GetAllClasses(const std::string& nameFilter)
    {
        std::vector<UEClass> out;
        const std::string filter = UENames::ToLower(nameFilter);
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.GetClassName() != "Class") continue;
            if (!filter.empty() && UENames::ToLower(obj.GetName()).find(filter) == std::string::npos) continue;
            out.push_back(UEClass(o));
        }
        return out;
    }

    int NumObjects() { return ObjectArray::Num(); }

    UEObject GetObjectByIndex(int index) { return UEObject(ObjectArray::GetByIndex(index)); }

    void ForEachObject(const std::function<void(UEObject)>& fn)
    {
        if (!fn) return;
        ObjectArray::ForEach([&](void* o) { fn(UEObject(o)); });
    }

    bool IsGameThread() { return Hooking::IsGameThread(); }

    void Dispatch(std::function<void()> action)
    {
        Hooking::RunOnGameThread(std::move(action));
    }

    bool DispatchSync(std::function<void()> action, int timeoutMs)
    {
        return Hooking::RunOnGameThreadSync(std::move(action), timeoutMs);
    }

    void DumpClassSummary(int topN)
    {
        std::unordered_map<std::string, int> counts;
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            std::string cn = UEObject(o).GetClassName();
            if (!cn.empty()) counts[cn]++;
        }

        std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::printf("[UEHook] Class summary (%d distinct classes, %d objects):\n",
                    (int)sorted.size(), count);
        for (int i = 0; i < topN && i < (int)sorted.size(); ++i)
            std::printf("   %6d  %s\n", sorted[i].second, sorted[i].first.c_str());
    }

    // Print a couple of readable values for a property, best-effort by type.
    static void PrintPropertyHint(UEObject obj, const UEProperty& p)
    {
        const std::string type = p.GetTypeName();
        std::printf("      %-32s %s", p.GetName().c_str(), type.c_str());

        if (type == "FloatProperty")       std::printf(" = %.3f", obj.GetFloat(p.GetName()));
        else if (type == "DoubleProperty") std::printf(" = %.3f", obj.Get<double>(p.GetName()));
        else if (type == "IntProperty")    std::printf(" = %d", obj.GetInt(p.GetName()));
        else if (type == "BoolProperty")   std::printf(" = %s", obj.GetBool(p.GetName()) ? "true" : "false");
        else if (type == "ObjectProperty") { UEObject v = obj.GetObjectProp(p.GetName());
                                             std::printf(" = %s", v ? v.GetName().c_str() : "null"); }
        else if (type == "StrProperty" || type == "NameProperty")
                                             std::printf(" = \"%s\"", obj.GetStringProp(p.GetName()).c_str());
        std::printf("\n");
    }

    void DumpObjects(const std::string& classFilter, int max, int hintFields)
    {
        int shown = 0;
        const int32 count = ObjectArray::Num();

        std::printf("[UEHook] Objects matching \"%s\":\n",
                    classFilter.empty() ? "*" : classFilter.c_str());

        for (int32 i = 0; i < count && shown < max; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;

            UEObject obj(o);
            std::string cn = obj.GetClassName();
            if (cn.empty()) continue;
            if (!classFilter.empty() && cn.find(classFilter) == std::string::npos) continue;

            std::printf("  [%d] %s  (class %s)\n", obj.GetIndex(), obj.GetName().c_str(), cn.c_str());

            // Show a few properties of this object's class as field hints.
            int printed = 0;
            UEClass cls = obj.GetClass();
            for (const UEProperty& p : cls.GetProperties())
            {
                if (printed++ >= hintFields) break;
                PrintPropertyHint(obj, p);
            }
            ++shown;
        }
        std::printf("[UEHook] (%d shown)\n", shown);
    }

    void DumpObjectValues(const std::string& classFilter, int maxProps)
    {
        const int32 count = ObjectArray::Num();
        UEObject target;       // preferred: a live instance
        UEObject fallback;     // any match (may be the CDO) if no live one exists
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            std::string cn = obj.GetClassName();
            if (cn.empty()) continue;
            if (!classFilter.empty() && cn.find(classFilter) == std::string::npos) continue;
            if (!fallback) fallback = obj;
            if (!IsDefaultObject(obj)) { target = obj; break; } // real live data
        }
        if (!target) target = fallback;

        if (!target)
        {
            std::printf("[UEHook] DumpObjectValues: no object matching \"%s\".\n", classFilter.c_str());
            return;
        }

        std::printf("[UEHook] %s (class %s) -- live property values:\n",
                    target.GetName().c_str(), target.GetClassName().c_str());

        int printed = 0;
        for (UEStruct s = target.GetClass(); s && printed < maxProps; s = s.GetSuper())
        {
            for (const UEProperty& p : s.GetProperties())
            {
                if (printed++ >= maxProps) break;
                UEValue v = target.GetValue(p.GetName());
                std::printf("   %-34s %-16s = %s\n",
                            p.GetName().c_str(), p.GetTypeName().c_str(), v.ToString().c_str());

                // Drill one level into structs and arrays to show it works.
                if (v.Kind == UEValueKind::Struct)
                {
                    UEStructRef sr(v);
                    int sub = 0;
                    for (const UEProperty& m : sr.GetMembers())
                    {
                        if (sub++ >= 4) break;
                        std::printf("        .%-27s %-12s = %s\n",
                                    m.GetName().c_str(), m.GetTypeName().c_str(),
                                    sr.GetValue(m.GetName()).ToString().c_str());
                    }
                }
                else if (v.Kind == UEValueKind::Array && v.Int > 0)
                {
                    UEArrayRef ar(v);
                    const int n = ar.Num() < 3 ? ar.Num() : 3;
                    for (int e = 0; e < n; ++e)
                        std::printf("        [%d] = %s\n", e, ar.At(e).ToString().c_str());
                }
                else if (v.Kind == UEValueKind::Set)
                {
                    UESetRef sr(v);
                    const int n = sr.Num() < 3 ? sr.Num() : 3;
                    std::printf("        (%d elements)\n", sr.Num());
                    for (int e = 0; e < n; ++e)
                        std::printf("        {%d} = %s\n", e, sr.At(e).ToString().c_str());
                }
                else if (v.Kind == UEValueKind::Map)
                {
                    UEMapRef mr(v);
                    const int n = mr.Num() < 3 ? mr.Num() : 3;
                    std::printf("        (%d pairs)\n", mr.Num());
                    for (int e = 0; e < n; ++e)
                        std::printf("        %s => %s\n",
                                    mr.KeyAt(e).ToString().c_str(),
                                    mr.ValueAt(e).ToString().c_str());
                }
            }
        }
        std::printf("[UEHook] (%d properties)\n", printed);
    }

    void ProbeContainers(int maxHits, int entriesPer)
    {
        const int32 count = ObjectArray::Num();
        int setHits = 0, mapHits = 0;

        std::printf("[UEHook] Probing for populated TMap / TSet properties...\n");

        for (int32 i = 0; i < count && (setHits < maxHits || mapHits < maxHits); ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            UEClass cls = obj.GetClass();
            if (!cls) continue;

            int guard = 0;
            for (UEStruct s = cls; s && guard++ < 64; s = s.GetSuper())
            {
                for (const UEProperty& p : s.GetProperties())
                {
                    const std::string t = p.GetTypeName();
                    if (t != "MapProperty" && t != "SetProperty") continue;

                    UEValue v = obj.GetValue(p.GetName());

                    if (v.Kind == UEValueKind::Set && setHits < maxHits)
                    {
                        UESetRef sr(v);
                        if (sr.Num() <= 0) continue;
                        std::printf("  [set] %s::%s  (%d elems, type %s)\n",
                                    obj.GetName().c_str(), p.GetName().c_str(),
                                    sr.Num(), sr.ElementType().c_str());
                        const int n = sr.Num() < entriesPer ? sr.Num() : entriesPer;
                        for (int e = 0; e < n; ++e)
                            std::printf("        {%d} = %s\n", e, sr.At(e).ToString().c_str());
                        ++setHits;
                    }
                    else if (v.Kind == UEValueKind::Map && mapHits < maxHits)
                    {
                        UEMapRef mr(v);
                        if (mr.Num() <= 0) continue;
                        std::printf("  [map] %s::%s  (%d pairs, %s -> %s)\n",
                                    obj.GetName().c_str(), p.GetName().c_str(),
                                    mr.Num(), mr.KeyType().c_str(), mr.ValueType().c_str());
                        const int n = mr.Num() < entriesPer ? mr.Num() : entriesPer;
                        for (int e = 0; e < n; ++e)
                            std::printf("        %s => %s\n",
                                        mr.KeyAt(e).ToString().c_str(),
                                        mr.ValueAt(e).ToString().c_str());
                        ++mapHits;
                    }
                }
            }
        }
        std::printf("[UEHook] ProbeContainers: %d set(s), %d map(s) shown.\n", setHits, mapHits);
    }
}
