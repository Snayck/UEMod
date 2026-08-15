#pragma once

/*
 * UEHook - a universal UE4/UE5 reflection + hooking backend.
 *
 * Link this static library into your own injected DLL, then:
 *
 *     #include "UEHook.h"
 *     if (UE::Initialize()) {
 *         UEObject pawn = UE::GetObjectsOfClass("Pawn").front();
 *         float hp = pawn.GetFloat("Health");
 *         pawn.SetBool("bCanBeDamaged", false);
 *         Hooking::AddPostByName("ReceiveDamage",
 *             [](Hooking::HookContext& ctx) {
 *                 float dmg = ctx.Params.Get<float>("DamageAmount");
 *             });
 *     }
 *
 * Offsets are auto-detected at Initialize(); anything you set in UEHookConfig
 * (>= 0, or a non-null pointer) overrides detection for stubborn games.
 */

#include "Unreal/UnrealTypes.h"
#include "Reflection/CoreUObject.h"
#include "Core/Hooking.h"

#include <string>
#include <vector>
#include <functional>

struct UEHookConfig
{
    // Struct-member offset overrides. -1 means "auto-detect".
    int itemSize              = -1;
    int classOffset           = -1;
    int nameOffset            = -1;
    int outerOffset           = -1;
    int superOffset           = -1;
    int childrenOffset        = -1;
    int childPropertiesOffset = -1;
    int propOffset            = -1;

    // -1 = auto, 0 = force UProperty world, 1 = force FProperty world.
    int useFProperty = -1;

    // ProcessEvent override (either is enough). Address wins over index.
    void* processEventAddr  = nullptr;
    int   processEventIndex = -1;

    // Force the GObjects/GNames layout flags (-1 = auto from detector).
    int isChunked   = -1; // 0/1
    int isFNamePool = -1; // 0/1

    bool installHook = true; // install the ProcessEvent detour at init
    bool verbose     = false; // print detection results to stdout
};

namespace UE
{
    bool Initialize(const UEHookConfig& cfg = UEHookConfig{});
    void Shutdown();
    bool IsInitialized();

    // Globals.
    UEObject GetEngine();
    UEObject GetWorld();

    // Lookups.
    UEObject   FindObject(const std::string& name, const std::string& className = "");
    UEClass    FindClass(const std::string& name);
    UEFunction FindFunction(const std::string& name);

    // All objects whose class is (or derives from) `className`. By default this
    // skips class-default objects and archetypes (the `Default__*` templates), so
    // a node graph gets real live instances; pass includeDefaults=true to include
    // them (e.g. to read a class's default property values).
    std::vector<UEObject> GetObjectsOfClass(const std::string& className,
                                            bool includeDefaults = false);

    // Every UClass in the process, for populating a node editor's class palette.
    // Optionally filter to class names containing `nameFilter` (empty = all).
    std::vector<UEClass> GetAllClasses(const std::string& nameFilter = "");

    // Decode an FName comparison index to its string (e.g. reading an FName that
    // lives inside a raw struct param, like FKey::KeyName inside an FKeyEvent).
    std::string ResolveName(int32 comparisonIndex);

    // Raw GObjects access.
    int      NumObjects();
    UEObject GetObjectByIndex(int index);
    void     ForEachObject(const std::function<void(UEObject)>& fn);

    // ---- game-thread dispatch -----------------------------------------------
    // A UI/other thread must not call UFunctions or write live objects directly;
    // route that work here and it runs on the game thread (drained by the
    // ProcessEvent hook, which must be installed). Reads are generally safe off-
    // thread; calls (CallByName/ProcessEvent) and writes should go through these.
    bool IsGameThread();
    void Dispatch(std::function<void()> action);                       // fire-and-forget
    bool DispatchSync(std::function<void()> action, int timeoutMs = 5000); // blocks

    // Run `fn` on the game thread, block until done, and return its result
    // (a default-constructed value on timeout). Ideal for a node that calls a
    // UFunction and needs the boxed return on the UI thread:
    //     UEValue r = UE::DispatchResult([&]{ return pc.CallByName("Foo", args); });
    template<typename Fn>
    auto DispatchResult(Fn&& fn, int timeoutMs = 5000) -> decltype(fn())
    {
        decltype(fn()) result{};
        Hooking::RunOnGameThreadSync([&]{ result = fn(); }, timeoutMs);
        return result;
    }

    // ---- discovery helpers (print to stdout) --------------------------------
    // Count objects per class and print the `topN` most common classes.
    // Great first look at an unknown game: shows what class names exist.
    void DumpClassSummary(int topN = 40);

    // List up to `max` objects whose class name contains `classFilter`
    // (empty = all), each with a few property hints (name : type = value).
    void DumpObjects(const std::string& classFilter = "", int max = 50, int hintFields = 5);

    // Print every property of the first object whose class name contains
    // `classFilter`, as "name : type = value" using the dynamic GetValue path.
    // Prefers a live instance over the class-default object. A direct test that
    // runtime-typed reads work end to end.
    void DumpObjectValues(const std::string& classFilter, int maxProps = 200);

    // Scan every object for populated TMap / TSet properties and print the first
    // `maxHits` of each, with a few entries. Direct live validation that the
    // container element strides/offsets resolve correctly.
    void ProbeContainers(int maxHits = 6, int entriesPer = 4);
}
