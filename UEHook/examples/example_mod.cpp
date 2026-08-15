// Example mod DLL built on the UEHook static library.
//
// Inject this into a UE4/UE5 game. It initializes the reflection backend,
// prints some discovered objects, reads a property, and installs a hook.

#include "UEHook.h"

#include <Windows.h>
#include <cstdio>
#include <thread>
#include <chrono>

// ---- Hook callbacks ---------------------------------------------------------
// One signature for everything: void(Hooking::HookContext&). These are plain
// named functions (no inline lambdas). std::function accepts them directly, so
// registration is just `Hooking::AddPreByName("Name", &Callback)`.

// Extract the pressed key from an FKeyEvent param. The only FName inside a
// FKeyEvent is FKey::KeyName, so we scan the struct for the first FName that
// resolves to a real key name ("W", "SpaceBar", "Enter", "Gamepad_..."). The
// offset is cached after the first hit so later calls are a single read.
static std::string DecodeKey(Hooking::HookContext& ctx)
{
    const int32 evtOff = ctx.Params.OffsetOf("InKeyEvent");
    if (evtOff < 0 || !ctx.RawParams)
        return "";
    const uintptr_t evt = reinterpret_cast<uintptr_t>(ctx.RawParams) + evtOff;

    static int keyNameOff = -1; // cached offset of FKey::KeyName within FKeyEvent
    auto tryAt = [&](int o) -> std::string
    {
        int32 idx = 0;
        if (!SafeMemory::Read<int32>(evt + o, &idx) || idx <= 0)
            return "";
        std::string s = UE::ResolveName(idx);
        if (s.empty() || s.size() > 48 || s == "None")
            return "";
        return s;
    };

    if (keyNameOff >= 0)
    {
        std::string s = tryAt(keyNameOff);
        if (!s.empty()) return s;
        keyNameOff = -1; // layout changed unexpectedly; re-scan
    }
    for (int o = 0; o <= 0x28; o += 4)
    {
        std::string s = tryAt(o);
        if (!s.empty()) { keyNameOff = o; return s; }
    }
    return "";
}

// Fires on any key press inside a menu widget.
static void OnMenuKey(Hooking::HookContext& ctx)
{
    const std::string key = DecodeKey(ctx);
    std::printf("[key] %-10s on %s\n",
                key.empty() ? "?" : key.c_str(), ctx.Caller.GetClassName().c_str());

    // Example: consume the Escape key so the game never sees it.
    // if (key == "Escape") ctx.ShouldCall = false;
}

// Fires when a menu button is activated -- by mouse click OR keyboard/gamepad,
// because CommonUI routes them all through this one function.
static void OnButtonActivated(Hooking::HookContext& ctx)
{
    std::printf("[click] %s\n", ctx.Caller.GetName().c_str());
    // ctx.ShouldCall = false;   // <- uncomment to SWALLOW the activation
}

// Mouse wheel over the menu -- a real input event through ProcessEvent.
static void OnWheel(Hooking::HookContext& ctx)
{
    std::printf("[wheel] on %s\n", ctx.Caller.GetName().c_str());
}

static void RunMod()
{
    // Give the game a moment to finish loading its object system.
    std::this_thread::sleep_for(std::chrono::seconds(5));

    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);

    UEHookConfig cfg;
    cfg.verbose = true;      // print detection results
    cfg.installHook = true;  // detour ProcessEvent

    if (!UE::Initialize(cfg))
    {
        std::printf("[mod] UEHook init failed.\n");
        return;
    }

    std::printf("[mod] Initialized. %d objects.\n", UE::NumObjects());

    // --- Dynamic (runtime-typed) API demo ------------------------------------
    // This is exactly what a node-editor runtime does: everything by string
    // name, with types resolved from reflection. No compile-time game knowledge.
    UE::DumpObjectValues("PlayerController", 40);

    // GetObjectsOfClass now returns LIVE instances (Default__ objects filtered
    // out). Grab the real PlayerController the game is driving.
    std::vector<UEObject> pcs = UE::GetObjectsOfClass("PlayerController");
    std::printf("[dyn] %d live PlayerController instance(s).\n", (int)pcs.size());

    if (!pcs.empty())
    {
        UEObject pc = pcs.front();
        std::printf("[dyn] using live instance: %s\n", pc.GetName().c_str());

        // Read a bool dynamically (boxed UEValue, type resolved from reflection),
        // flip it, and read back to prove the write round-trips.
        UEValue cursor = pc.GetValue("bShowMouseCursor");
        const bool want = !cursor.AsBool();
        pc.SetValue("bShowMouseCursor", UEValue::MakeBool(want));
        std::printf("[dyn] bShowMouseCursor %s -> set %s -> now %s\n",
                    cursor.ToString().c_str(), want ? "true" : "false",
                    pc.GetValue("bShowMouseCursor").ToString().c_str());

        // FName write round-trip: find any NameProperty up the hierarchy, then
        // write a KNOWN-EXISTING name string back through the FindIndex + set
        // path. "None" is always in the pool, so it proves string->FName resolve.
        bool didName = false;
        for (UEStruct s = pc.GetClass(); s && !didName; s = s.GetSuper())
        {
            for (const UEProperty& p : s.GetProperties())
            {
                if (p.GetTypeName() != "NameProperty") continue;
                const std::string field = p.GetName();
                UEValue before = pc.GetValue(field);
                bool wrote = pc.SetValue(field, UEValue::MakeString("None"));
                std::printf("[dyn] FName %s: was \"%s\", write \"None\" %s -> now \"%s\"\n",
                            field.c_str(), before.ToString().c_str(),
                            wrote ? "ok" : "FAILED",
                            pc.GetValue(field).ToString().c_str());
                didName = true;
                break;
            }
        }
    }

    // --- TMap / TSet live validation -----------------------------------------
    // Hunt the whole object graph for populated maps/sets and print a few
    // entries, so we can confirm the container strides resolve correctly.
    UE::ProbeContainers(6, 4);

    // --- Introspection demo (what the node editor uses to build pins) --------
    // Describe pins statically from reflection: enum dropdowns, object-pin
    // target classes, struct-pin types -- all without any live value.
    std::printf("[introspect] %d total classes in process.\n",
                (int)UE::GetAllClasses().size());

    if (UEClass pcClass = UE::FindClass("PlayerController"))
    {
        bool didEnum = false, didObj = false, didStruct = false;
        for (const UEProperty& p : pcClass.GetAllProperties())
        {
            const std::string t = p.GetTypeName();

            if (!didEnum && (t == "EnumProperty" || t == "ByteProperty"))
            {
                UEEnum en = p.GetEnum();
                if (en)
                {
                    auto opts = en.GetShortNames();
                    if (!opts.empty())
                    {
                        std::printf("[introspect] enum pin %s (%s) options: ",
                                    p.GetName().c_str(), en.GetName().c_str());
                        int shown = 0;
                        for (auto& o : opts)
                        {
                            if (shown++ >= 6) break;
                            std::printf("%s=%lld ", o.first.c_str(), (long long)o.second);
                        }
                        std::printf("\n");
                        didEnum = true;
                    }
                }
            }
            else if (!didObj && t == "ObjectProperty")
            {
                UEClass tc = p.GetObjectClass();
                if (tc)
                {
                    std::printf("[introspect] object pin %s -> target class %s\n",
                                p.GetName().c_str(), tc.GetName().c_str());
                    didObj = true;
                }
            }
            else if (!didStruct && t == "StructProperty")
            {
                UEStruct st = p.GetStructType();
                if (st)
                {
                    std::printf("[introspect] struct pin %s -> type %s\n",
                                p.GetName().c_str(), st.GetName().c_str());
                    didStruct = true;
                }
            }
        }
    }

    if (!Hooking::IsInstalled())
    {
        std::printf("[mod] ProcessEvent hook NOT installed -- hooking unavailable.\n");
        return;
    }
    std::printf("[mod] ProcessEvent hooked. Enabling call log.\n");
    std::printf("[mod] NOTE: ProcessEvent only sees script/Blueprint calls. Raw native\n"
                "      key presses won't show, but Blueprint input events, tick, and\n"
                "      draw/HUD functions will. Interact with the game to see them.\n\n");

    // Every unique UFunction that fires prints once as Class::Function.
    // Leave on while exploring; comment out once you know what to hook.
    Hooking::EnableCallLog(true);

    // --- Targeted hooks on confirmed input events ----------------------------
    // Register named callbacks (not inline lambdas). Same callback type for all.
    Hooking::AddPreByName("OnKeyDown", &OnMenuKey);
    Hooking::AddPreByName("OnHandleButtonBaseClicked", &OnButtonActivated);
    Hooking::AddPreByName("OnMouseWheel", &OnWheel);

    std::printf("[mod] Hooks armed: OnKeyDown, OnHandleButtonBaseClicked, OnMouseWheel.\n"
                "      Press keys / click / scroll in the menu to trigger them.\n\n");

    // --- Game-thread dispatch demo -------------------------------------------
    // This RunMod thread is NOT the game thread -- exactly the situation a UI
    // thread is in. Route work through Dispatch and it runs where UE expects it.
    std::printf("[dispatch] mod thread: IsGameThread=%d\n", UE::IsGameThread());
    bool ran = UE::DispatchSync([]
    {
        std::printf("[dispatch] task executing: IsGameThread=%d\n", UE::IsGameThread());
    }, 3000);
    std::printf("[dispatch] sync task %s\n", ran ? "completed on game thread" : "TIMED OUT");

    // Keep the mod thread alive so the hook stays installed.
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(60));
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        std::thread(RunMod).detach();
    }
    return TRUE;
}
