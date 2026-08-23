#include "calllogger.h"
#include "imgui.h"

#ifndef UEEDITOR_WITH_BACKEND

namespace CallLogger {
    void Init() {}
    void Shutdown() {}
    void Render() {
        if (!ImGui::Begin("Call Logger")) { ImGui::End(); return; }
        ImGui::TextDisabled("backend inactive (standalone)");
        ImGui::End();
    }
}

#else

#include "UEHook.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace CallLogger {

namespace {

constexpr size_t kMaxEntries = 1000;
constexpr size_t kMaxFrame   = 64 * 1024;

struct ParamInfo {
    std::string Name;
    std::string TypeName;
    bool IsOut = false;
    bool IsReturn = false;
};

struct FuncInfo {
    std::string ShortName;
    std::string FullName;
    int32 FrameSize = 0;
    bool HasReturn = false;
    std::vector<ParamInfo> Params;
};

struct Entry {
    uint64_t Seq = 0;
    void* FuncPtr = nullptr;
    std::string CallerName;
    std::string FuncShort;
    std::string FuncFull;
    std::vector<ParamInfo> Params;
    std::vector<uint8_t> Frame;
    std::string Return;
    bool ReturnRead = false;
    bool ViewsInitialized = false;
    std::vector<std::string> Values;
};

std::atomic<bool> g_Capturing{ false };
std::mutex g_Mutex;
std::list<Entry> g_Entries;
std::unordered_map<uint64_t, std::list<Entry>::iterator> g_BySeq;
uint64_t g_NextSeq = 1;

char g_Whitelist[256] = "";
char g_Blacklist[256] = "";

std::unordered_map<void*, FuncInfo> g_FuncCache;   // game thread only
thread_local std::vector<uint64_t> tl_PostSeqs;

std::string ToLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

bool TokenMatch(const std::string& lowerName, const char* filter) {
    if (!filter || !*filter) return false;
    const char* p = filter;
    while (*p) {
        std::string tok;
        while (*p && *p != ',') {
            char c = *p++;
            if (c >= 'A' && c <= 'Z') c += 32;
            tok += c;
        }
        if (*p == ',') ++p;
        if (!tok.empty() && lowerName.find(tok) != std::string::npos) return true;
    }
    return false;
}

bool PassesFilters(const std::string& lowerShort) {
    if (g_Whitelist[0] && !TokenMatch(lowerShort, g_Whitelist)) return false;
    if (g_Blacklist[0] && TokenMatch(lowerShort, g_Blacklist)) return false;
    return true;
}

const FuncInfo& CachedFunc(UEFunction fn) {
    auto it = g_FuncCache.find(fn.GetAddress());
    if (it != g_FuncCache.end()) return it->second;

    FuncInfo fi;
    fi.ShortName = fn.GetName();
    fi.FullName  = fn.GetFullName();
    int32 sz = fn.GetPropertiesSize();
    fi.FrameSize = (sz >= 0 && (size_t)sz <= kMaxFrame) ? sz : 0;
    for (const UEProperty& p : fn.GetParams()) {
        EPropertyFlags fl = p.GetPropertyFlags();
        ParamInfo pi;
        pi.Name = p.GetName();
        pi.TypeName = p.GetTypeName();
        pi.IsOut    = (fl & EPropertyFlags::OutParm) != 0;
        pi.IsReturn = (fl & EPropertyFlags::ReturnParm) != 0;
        if (pi.IsReturn) fi.HasReturn = true;
        fi.Params.push_back(std::move(pi));
    }
    return g_FuncCache.emplace(fn.GetAddress(), std::move(fi)).first->second;
}

Hooking::CallSink g_Sink;

void SerializeValues(Entry& e) {
    e.Values.assign(e.Params.size(), "?");
    if (!e.Frame.empty() && e.FuncPtr) {
        UEParams pr(UEFunction(e.FuncPtr), e.Frame.data());
        if (pr)
            for (size_t i = 0; i < e.Params.size(); ++i)
                e.Values[i] = pr.GetValue(e.Params[i].Name).ToString();
    }
    e.ViewsInitialized = true;
}

struct Group {
    std::string Label;
    std::vector<uint64_t> Seqs;
};

} // namespace

void Init() {
    g_Sink.pre = [](Hooking::HookContext& ctx) -> bool {
        if (!g_Capturing.load(std::memory_order_relaxed)) return false;
        const FuncInfo& fi = CachedFunc(ctx.Function);
        if (!PassesFilters(ToLower(fi.ShortName))) return false;

        Entry e;
        e.Seq = g_NextSeq++;
        e.FuncPtr = ctx.Function.GetAddress();
        e.CallerName = ctx.Caller ? ctx.Caller.GetName() : "?";
        e.FuncShort = fi.ShortName;
        e.FuncFull = fi.FullName;
        e.Params = fi.Params;
        if (ctx.RawParams && fi.FrameSize > 0)
            e.Frame.assign(static_cast<uint8*>(ctx.RawParams),
                           static_cast<uint8*>(ctx.RawParams) + fi.FrameSize);

        uint64_t seq = e.Seq;
        {
            std::lock_guard<std::mutex> lk(g_Mutex);
            g_BySeq[seq] = g_Entries.emplace(g_Entries.end(), std::move(e));
            if (g_Entries.size() > kMaxEntries) {
                g_BySeq.erase(g_Entries.front().Seq);
                g_Entries.pop_front();
            }
        }
        if (fi.HasReturn) {
            tl_PostSeqs.push_back(seq);
            return true;
        }
        return false;
    };
    g_Sink.post = [](Hooking::HookContext& ctx) {
        if (tl_PostSeqs.empty() || !ctx.Result) return;
        uint64_t seq = tl_PostSeqs.back();
        tl_PostSeqs.pop_back();
        UEProperty ret = ctx.Function.GetReturnProperty();
        if (!ret) return;
        std::string val = ctx.Params.GetValue(ret.GetName()).ToString();
        std::lock_guard<std::mutex> lk(g_Mutex);
        auto it = g_BySeq.find(seq);
        if (it != g_BySeq.end()) {
            it->second->Return = val;
            it->second->ReturnRead = true;
        }
    };
    Hooking::SetCallSink(&g_Sink);
}

void Shutdown() {
    Hooking::SetCallSink(nullptr);
    std::lock_guard<std::mutex> lk(g_Mutex);
    g_Entries.clear();
    g_BySeq.clear();
}

void Render() {
    if (!ImGui::Begin("Call Logger")) {
        ImGui::End();
        return;
    }

    bool capturing = g_Capturing.load();
    const char* startLabel = capturing ? "Pause"
                     : g_Entries.empty() ? "Start" : "Resume";
    if (ImGui::Button(startLabel))
        g_Capturing.store(!capturing);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        std::lock_guard<std::mutex> lk(g_Mutex);
        g_Entries.clear();
        g_BySeq.clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##wl", "whitelist", g_Whitelist, sizeof g_Whitelist);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##bl", "blacklist", g_Blacklist, sizeof g_Blacklist);
    ImGui::SameLine();
    size_t count;
    { std::lock_guard<std::mutex> lk(g_Mutex); count = g_Entries.size(); }
    ImGui::TextDisabled("%d/%d", (int)count, (int)kMaxEntries);

    ImGui::Separator();

    std::vector<Group> groups;
    {
        std::lock_guard<std::mutex> lk(g_Mutex);
        std::unordered_map<std::string, size_t> groupIdx;
        for (auto it = g_Entries.begin(); it != g_Entries.end(); ++it) {
            std::string key = it->FuncFull;
            auto git = groupIdx.find(key);
            if (git == groupIdx.end()) {
                Group grp;
                grp.Label = it->FuncFull.rfind("Function ", 0) == 0
                    ? it->FuncFull.substr(9) : it->FuncFull;
                groups.push_back(std::move(grp));
                git = groupIdx.emplace(key, groups.size() - 1).first;
            }
            groups[git->second].Seqs.push_back(it->Seq);
        }
    }

    ImGui::BeginChild("##log", ImVec2(0, 0), false);
    for (Group& grp : groups) {
        ImGui::PushID(grp.Label.c_str());
        if (ImGui::TreeNode(grp.Label.c_str(), "%s (%d)", grp.Label.c_str(), (int)grp.Seqs.size())) {
            std::lock_guard<std::mutex> lk(g_Mutex);
            for (auto sit = grp.Seqs.rbegin(); sit != grp.Seqs.rend(); ++sit) {
                auto eit = g_BySeq.find(*sit);
                if (eit == g_BySeq.end() || eit->second == g_Entries.end()) continue;
                Entry& e = *eit->second;
                ImGui::PushID((int)e.Seq);
                if (ImGui::TreeNode("#", "#%d  %s", (int)e.Seq, e.CallerName.c_str())) {
                    if (!e.ViewsInitialized) SerializeValues(e);
                    char full[512];
                    strncpy(full, e.FuncFull.c_str(), sizeof full); full[sizeof full - 1] = 0;
                    ImGui::PushItemWidth(-1);
                    ImGui::InputText("##full", full, sizeof full, ImGuiInputTextFlags_ReadOnly);
                    ImGui::PopItemWidth();
                    for (size_t i = 0; i < e.Params.size(); ++i) {
                        const ParamInfo& pi = e.Params[i];
                        ImGui::Bullet();
                        if (pi.IsReturn) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImColor(255, 90, 90), "%s %s = %s",
                                pi.TypeName.c_str(), pi.Name.c_str(),
                                e.ReturnRead ? e.Return.c_str() : "...");
                        } else {
                            if (pi.IsOut) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImColor(255, 200, 90), "&out");
                                ImGui::SameLine();
                            }
                            ImGui::TextColored(ImColor(120, 170, 255), "%s", pi.TypeName.c_str());
                            ImGui::SameLine();
                            ImGui::TextColored(ImColor(255, 190, 120), "%s", pi.Name.c_str());
                            ImGui::SameLine();
                            ImGui::TextUnformatted(e.Values[i].c_str());
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace CallLogger

#endif
