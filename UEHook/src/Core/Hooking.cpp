#include "Hooking.h"
#include "Engine/Offsets.h"
#include "Core/SafeMemory.h"
#include "Unreal/ObjectArray.h"
#include "ThirdParty/MinHook/MinHook.hpp"

#include <map>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <atomic>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>

namespace
{
    using PEFn = void(*)(void*, void*, void*);

    PEFn g_original = nullptr;
    std::atomic<bool> g_installed{ false };

    std::shared_mutex g_mutex;

    struct Entry { int handle; Hooking::HookCallback cb; };

    std::map<int32, std::vector<Entry>>  g_pre;
    std::map<int32, std::vector<Entry>>  g_post;
    std::map<int, std::pair<int32, bool>>   g_handleMap; // handle -> (funcIndex, isPre)
    std::atomic<int> g_nextHandle{ 1 };

    std::atomic<bool>        g_logAll{ false };
    std::mutex               g_logMutex;
    std::unordered_set<int32> g_logged;

    std::atomic<Hooking::CallSink*> g_sink{ nullptr };

    // ---- ProcessInternal (Blueprint VM) hook --------------------------------
    // ProcessEvent only sees calls entering via reflection. Calls made from
    // running Blueprint bytecode (EX_CallFunction) reach scripted functions
    // through UObject::ProcessInternal instead. Found without signatures:
    // every non-native UFunction's Func points at it.
    using PIFn = uint8(*)(void*, void*, void*);

    PIFn             g_piOriginal = nullptr;
    void*            g_piTarget = nullptr;
    std::atomic<bool> g_piInstalled{ false };
    std::atomic<int>  g_ffNodeOff{ -1 };   // FFrame::Node offset (-2 = calib failed)
    thread_local std::vector<void*> tl_ScriptedExec;   // PE-dispatched scripted funcs

    bool LooksLikeFunctionPtr(void* p) {
        if (!p || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(p))) return false;
        return UEObject(p).GetClassName() == "Function";
    }

    int CalibrateFrame(void* stack) {
        for (int off = 0; off <= 0x20; off += 8) {
            void* node = nullptr;
            if (!SafeMemory::Read<void*>(reinterpret_cast<uintptr_t>(stack) + off, &node))
                continue;
            if (!LooksLikeFunctionPtr(node)) continue;
            void* obj = nullptr;
            if (!SafeMemory::Read<void*>(reinterpret_cast<uintptr_t>(stack) + off + 8, &obj))
                continue;
            if (!obj || !SafeMemory::IsReasonable(reinterpret_cast<uintptr_t>(obj))) continue;
            if (UEObject(obj).GetClassName().empty()) continue;
            return off;
        }
        return -2;
    }

    void* FindProcessInternal() {
        std::unordered_map<void*, int> counts;
        void* best = nullptr;
        int bestCount = 0;
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.GetClassName() != "Function") continue;
            UEFunction fn(o);
            if (fn.HasFlags(EFunctionFlags::Native)) continue;
            void* f = fn.GetExecFunction();
            if (!f) continue;
            int& c = counts[f];
            if (++c > bestCount) { bestCount = c; best = f; }
        }
        return bestCount >= 8 ? best : nullptr;
    }

    uint8 ProcessInternalDetour(void* context, void* stackVoid, void* result)
    {
        static thread_local int depth = 0;

        if (stackVoid && depth <= 64 && g_sink.load(std::memory_order_relaxed))
        {
            int nodeOff = g_ffNodeOff.load(std::memory_order_relaxed);
            if (nodeOff == -1)
                g_ffNodeOff.store(nodeOff = CalibrateFrame(stackVoid), std::memory_order_relaxed);

            if (nodeOff >= 0)
            {
                uintptr_t sp = reinterpret_cast<uintptr_t>(stackVoid);
                void* node = nullptr;
                void* obj = nullptr;
                void* locals = nullptr;
                SafeMemory::Read<void*>(sp + nodeOff, &node);
                SafeMemory::Read<void*>(sp + nodeOff + 8, &obj);
                SafeMemory::Read<void*>(sp + nodeOff + 0x18, &locals);

                bool peDispatched = !tl_ScriptedExec.empty() && tl_ScriptedExec.back() == node;
                if (node && !peDispatched)
                {
                    if (Hooking::CallSink* sink = g_sink.load(std::memory_order_relaxed))
                    {
                        if (sink->pre)
                        {
                            UEFunction fn(node);
                            Hooking::HookContext ctx;
                            ctx.Caller    = UEObject(obj);
                            ctx.Function  = fn;
                            ctx.Params    = UEParams(fn, locals);
                            ctx.RawParams = locals;
                            ctx.FromVM    = true;
                            sink->pre(ctx);
                        }
                    }
                }
            }
        }

        ++depth;
        uint8 r = g_piOriginal(context, stackVoid, result);
        --depth;
        return r;
    }

    // ---- game-thread dispatch queue ----
    std::mutex                          g_taskMutex;
    std::vector<std::function<void()>>  g_tasks;
    std::atomic<unsigned long>          g_gameThreadId{ 0 };
    thread_local bool                   tl_draining = false;

    // Run all queued tasks on the current (game) thread. Guarded against nested
    // drains so a task that itself triggers ProcessEvent doesn't re-enter.
    void DrainTasks()
    {
        if (tl_draining) return;

        std::vector<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lk(g_taskMutex);
            if (g_tasks.empty()) return;
            local.swap(g_tasks);
        }

        tl_draining = true;
        for (auto& t : local)
        {
            if (!t) continue;
            try { t(); } catch (...) {}
        }
        tl_draining = false;
    }

    int32 FuncIndex(void* func)
    {
        int32 idx = 0;
        if (func)
            SafeMemory::Read<int32>(reinterpret_cast<uintptr_t>(func) + Off::UObject::Index, &idx);
        return idx;
    }

    // Locate the return-value buffer inside params, if the function has one.
    void* ComputeReturn(UEFunction func, void* params)
    {
        if (!params) return nullptr;
        for (const UEProperty& p : func.GetProperties())
        {
            if (p.GetPropertyFlags() & EPropertyFlags::ReturnParm)
                return static_cast<uint8*>(params) + p.GetOffset();
        }
        return nullptr;
    }

    void ProcessEventDetour(void* caller, void* func, void* params)
    {
        // Cap pathological recursion depth on this thread.
        static thread_local int depth = 0;

        // This detour runs on the game thread: record it, then drain any work the
        // UI thread queued so it executes here where UE expects it.
        g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
        DrainTasks();

        if (!func || depth > 64)
        {
            if (g_original) g_original(caller, func, params);
            return;
        }

        const int32 idx = FuncIndex(func);

        if (g_logAll.load(std::memory_order_relaxed) && idx > 0)
        {
            bool isNew = false;
            {
                std::lock_guard<std::mutex> lk(g_logMutex);
                isNew = g_logged.insert(idx).second;
            }
            if (isNew)
            {
                UEObject   c(caller);
                UEFunction f(func);
                std::printf("[PE] %s::%s\n", c.GetClassName().c_str(), f.GetName().c_str());
            }
        }

        std::vector<Entry> pre;
        std::vector<Entry> post;
        {
            std::shared_lock lock(g_mutex);
            auto itPre  = g_pre.find(idx);
            auto itPost = g_post.find(idx);
            if (itPre  != g_pre.end())  pre  = itPre->second;   // copy so we don't hold the lock during callbacks
            if (itPost != g_post.end()) post = itPost->second;
        }

        UEFunction ueFunc(func);

        Hooking::HookContext ctx;
        ctx.Caller    = UEObject(caller);
        ctx.Function  = ueFunc;
        ctx.Params    = UEParams(ueFunc, params);
        ctx.RawParams = params;

        ++depth;

        bool sinkWantsPost = false;
        if (Hooking::CallSink* sink = g_sink.load(std::memory_order_relaxed))
        {
            if (sink->pre)
                sinkWantsPost = sink->pre(ctx);
        }

        for (auto& e : pre)
            e.cb(ctx);

        if (ctx.ShouldCall && g_original)
        {
            // mark scripted dispatches so the ProcessInternal detour doesn't
            // double-report calls that originated here
            bool scripted = !ueFunc.HasFlags(EFunctionFlags::Native);
            if (scripted) tl_ScriptedExec.push_back(func);
            g_original(caller, func, params);
            if (scripted) tl_ScriptedExec.pop_back();
        }

        if (!post.empty() || sinkWantsPost)
        {
            ctx.IsPost = true;
            ctx.Result = ComputeReturn(ueFunc, params);
            for (auto& e : post)
                e.cb(ctx);
            if (sinkWantsPost)
            {
                if (Hooking::CallSink* sink = g_sink.load(std::memory_order_relaxed))
                {
                    if (sink->post)
                        sink->post(ctx);
                }
            }
        }

        --depth;
    }

    // Matches "GetMousePosition", "/Script/Engine.PlayerController:GetMousePosition",
    // or "Function /Script/..." (the call logger's copyable format).
    bool MatchesFuncName(const UEObject& obj, const std::string& requested)
    {
        std::string r = requested;
        if (r.rfind("Function ", 0) == 0) r = r.substr(9);
        if (r.find(':') != std::string::npos || r.find('/') != std::string::npos)
            return UENames::EqualsCI(obj.GetPathName(), r);
        return UENames::EqualsCI(obj.GetName(), r);
    }

    UEFunction FindFunctionByName(const std::string& name)
    {
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.GetClassName() == "Function" && MatchesFuncName(obj, name))
                return UEFunction(o);
        }
        return UEFunction();
    }
}

namespace Hooking
{
    bool Install()
    {
        if (g_installed)
            return true;

        void* target = Off::InSDK::ProcessEvent::FuncPtr;
        if (!target)
            return false;

        MinHook::MH_STATUS st = MinHook::MH_Initialize();
        if (st != MinHook::MH_STATUS::MH_OK && st != MinHook::MH_STATUS::MH_ERROR_ALREADY_INITIALIZED)
            return false;

        if (MinHook::MH_CreateHook(target, reinterpret_cast<LPVOID>(&ProcessEventDetour),
                                   reinterpret_cast<LPVOID*>(&g_original)) != MinHook::MH_STATUS::MH_OK)
            return false;

        if (MinHook::MH_EnableHook(target) != MinHook::MH_STATUS::MH_OK)
        {
            MinHook::MH_RemoveHook(target);
            return false;
        }

        // Manual UEObject::ProcessEvent calls should bypass our detour.
        Off::InSDK::ProcessEvent::Callable = reinterpret_cast<void*>(g_original);
        g_installed = true;

        g_piTarget = FindProcessInternal();
        if (g_piTarget)
        {
            if (MinHook::MH_CreateHook(g_piTarget, reinterpret_cast<LPVOID>(&ProcessInternalDetour),
                                       reinterpret_cast<LPVOID*>(&g_piOriginal)) == MinHook::MH_STATUS::MH_OK
                && MinHook::MH_EnableHook(g_piTarget) == MinHook::MH_STATUS::MH_OK)
            {
                g_piInstalled = true;
            }
            else
            {
                MinHook::MH_RemoveHook(g_piTarget);
                g_piOriginal = nullptr;
                g_piTarget = nullptr;
            }
        }

        if (g_piInstalled.load())
            std::printf("[UEHook] ProcessInternal hooked at %p (VM calls visible)\n", g_piTarget);
        else
            std::printf("[UEHook] ProcessInternal not found; Blueprint-internal calls invisible\n");
        return true;
    }

    void Uninstall()
    {
        if (!g_installed)
            return;

        if (g_piInstalled.exchange(false))
        {
            MinHook::MH_DisableHook(g_piTarget);
            MinHook::MH_RemoveHook(g_piTarget);
            g_piOriginal = nullptr;
            g_piTarget = nullptr;
            g_ffNodeOff.store(-1, std::memory_order_relaxed);
        }

        void* target = Off::InSDK::ProcessEvent::FuncPtr;
        MinHook::MH_DisableHook(target);
        MinHook::MH_RemoveHook(target);
        MinHook::MH_Uninitialize();

        {
            std::unique_lock lock(g_mutex);
            g_pre.clear();
            g_post.clear();
            g_handleMap.clear();
        }
        {
            // Drop any pending dispatch work; sync waiters will simply time out.
            std::lock_guard<std::mutex> lk(g_taskMutex);
            g_tasks.clear();
        }

        Off::InSDK::ProcessEvent::Callable = Off::InSDK::ProcessEvent::FuncPtr;
        g_original = nullptr;
        g_installed = false;
    }

    bool IsInstalled() { return g_installed; }

    bool IsGameThread()
    {
        const unsigned long id = g_gameThreadId.load(std::memory_order_relaxed);
        return id != 0 && id == GetCurrentThreadId();
    }

    void RunOnGameThread(std::function<void()> task)
    {
        if (!task) return;
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_tasks.push_back(std::move(task));
    }

    bool RunOnGameThreadSync(std::function<void()> task, int timeoutMs)
    {
        if (!task) return false;

        // Already on the game thread (e.g. called from inside a hook): just run it.
        if (IsGameThread())
        {
            try { task(); } catch (...) {}
            return true;
        }

        auto prom = std::make_shared<std::promise<void>>();
        std::future<void> fut = prom->get_future();

        RunOnGameThread([task = std::move(task), prom]() mutable
        {
            try { task(); } catch (...) {}
            prom->set_value();
        });

        return fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
    }

    void EnableCallLog(bool enabled)
    {
        if (enabled)
        {
            std::lock_guard<std::mutex> lk(g_logMutex);
            g_logged.clear();
        }
        g_logAll = enabled;
    }

    void SetCallSink(CallSink* sink)
    {
        g_sink.store(sink, std::memory_order_release);
    }

    int AddPre(UEFunction func, HookCallback cb)
    {
        if (!func || !cb) return 0;
        const int32 idx = func.GetIndex();
        const int handle = g_nextHandle++;

        std::unique_lock lock(g_mutex);
        g_pre[idx].push_back({ handle, std::move(cb) });
        g_handleMap[handle] = { idx, true };
        return handle;
    }

    int AddPost(UEFunction func, HookCallback cb)
    {
        if (!func || !cb) return 0;
        const int32 idx = func.GetIndex();
        const int handle = g_nextHandle++;

        std::unique_lock lock(g_mutex);
        g_post[idx].push_back({ handle, std::move(cb) });
        g_handleMap[handle] = { idx, false };
        return handle;
    }

    int AddPreByName(const std::string& funcName, HookCallback cb)
    {
        UEFunction fn = FindFunctionByName(funcName);
        return fn ? AddPre(fn, std::move(cb)) : 0;
    }

    int AddPostByName(const std::string& funcName, HookCallback cb)
    {
        UEFunction fn = FindFunctionByName(funcName);
        return fn ? AddPost(fn, std::move(cb)) : 0;
    }

    bool Remove(int handle)
    {
        std::unique_lock lock(g_mutex);
        auto it = g_handleMap.find(handle);
        if (it == g_handleMap.end())
            return false;

        const int32 idx = it->second.first;
        const bool isPre = it->second.second;

        if (isPre)
        {
            auto& vec = g_pre[idx];
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                        [handle](const Entry& e) { return e.handle == handle; }), vec.end());
            if (vec.empty()) g_pre.erase(idx);
        }
        else
        {
            auto& vec = g_post[idx];
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                        [handle](const Entry& e) { return e.handle == handle; }), vec.end());
            if (vec.empty()) g_post.erase(idx);
        }

        g_handleMap.erase(it);
        return true;
    }
}
