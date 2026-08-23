#pragma once

#include "Reflection/CoreUObject.h"
#include <functional>
#include <string>

/*
 * ProcessEvent-based function hooking.
 *
 * A single detour on UObject::ProcessEvent dispatches to per-UFunction
 * callbacks, keyed by the function's InternalIndex. Registration is thread-safe;
 * the detour dispatches under a shared lock. Manual UEObject::ProcessEvent calls
 * route through the trampoline, so they never re-enter the detour.
 */
namespace Hooking
{
    // Everything a hook callback needs, in one object.
    struct HookContext
    {
        UEObject   Caller;              // the object ProcessEvent was called on
        UEFunction Function;            // the UFunction being called
        UEParams   Params;              // by-name access to the param frame
        void*      RawParams = nullptr; // the raw params pointer, if you need it

        bool  ShouldCall = true;        // PRE only: set false to skip the original
        void* Result     = nullptr;     // POST only: the return-value buffer
        bool  IsPost     = false;       // false in pre-hooks, true in post-hooks
        bool  FromVM     = false;       // true when dispatched via ProcessInternal
                                        // (a call made from Blueprint bytecode)
    };

    // One callback type for both pre- and post-hooks. std::function, so a plain
    // function, a lambda, or a std::bind all work:
    //     void MyHook(Hooking::HookContext& ctx) { ... }
    //     Hooking::AddPreByName("OnKeyDown", &MyHook);
    using HookCallback = std::function<void(HookContext&)>;

    bool Install();      // detours Off::InSDK::ProcessEvent::FuncPtr
    void Uninstall();
    bool IsInstalled();

    // ---- game-thread dispatch -----------------------------------------------
    // UE functions (ProcessEvent/CallByName) and most writes must run on the
    // game thread. A separate UI thread posts work here; the ProcessEvent detour
    // drains the queue each call, so the work executes on the game thread.
    // Requires the hook to be installed (that's what pumps the queue).

    // True if the caller is currently on the game (ProcessEvent) thread.
    bool IsGameThread();

    // Queue a task to run on the game thread. Returns immediately (fire-and-forget).
    void RunOnGameThread(std::function<void()> task);

    // Queue a task and block until it has run, or `timeoutMs` elapses. Returns
    // true if it ran. If already on the game thread, runs inline immediately.
    bool RunOnGameThreadSync(std::function<void()> task, int timeoutMs = 5000);

    // Debug/discovery: when enabled, every unique UFunction dispatched through
    // ProcessEvent is printed once as "CallerClass::FunctionName". This is the
    // easiest way to see the hook working and to find functions worth hooking.
    // Enabling resets the "already seen" set.
    void EnableCallLog(bool enabled);

    // Register a callback. Returns a handle (>0) for Remove(), or 0 on failure.
    // Pre-hooks run before the original (and can suppress it); post-hooks run
    // after (and can read/override the return value).
    int AddPre(UEFunction func, HookCallback cb);
    int AddPost(UEFunction func, HookCallback cb);

    // Convenience: resolve the first UFunction with this name, then register.
    int AddPreByName(const std::string& funcName, HookCallback cb);
    int AddPostByName(const std::string& funcName, HookCallback cb);

    bool Remove(int handle);

    // Observes every ProcessEvent call (one slot; nullptr detaches).
    // pre runs before the original -- return true to also receive post.
    struct CallSink
    {
        std::function<bool(HookContext&)> pre;
        std::function<void(HookContext&)> post;   // post: ctx.Result = return buffer
    };
    void SetCallSink(CallSink* sink);
}
