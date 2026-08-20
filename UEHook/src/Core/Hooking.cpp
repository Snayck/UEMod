#include "Hooking.h"
#include "Engine/Offsets.h"
#include "Core/SafeMemory.h"
#include "Unreal/ObjectArray.h"
#include "ThirdParty/MinHook/MinHook.hpp"

#include <map>
#include <vector>
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

        for (auto& e : pre)
            e.cb(ctx);

        if (ctx.ShouldCall && g_original)
            g_original(caller, func, params);

        if (!post.empty())
        {
            ctx.IsPost = true;
            ctx.Result = ComputeReturn(ueFunc, params);
            for (auto& e : post)
                e.cb(ctx);
        }

        --depth;
    }

    UEFunction FindFunctionByName(const std::string& name)
    {
        const int32 count = ObjectArray::Num();
        for (int32 i = 0; i < count; ++i)
        {
            void* o = ObjectArray::GetByIndex(i);
            if (!o) continue;
            UEObject obj(o);
            if (obj.GetClassName() == "Function" && UENames::EqualsCI(obj.GetName(), name))
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
        return true;
    }

    void Uninstall()
    {
        if (!g_installed)
            return;

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
