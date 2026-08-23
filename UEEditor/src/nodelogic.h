#pragma once
#include "graph.h"

// Execution layer; no-ops in the standalone .exe (UEEDITOR_WITH_BACKEND).
namespace Exec {
    void InitBackend();
    bool BackendReady();
    const char* BackendStatus();
    void RunScriptNode(Editor::Graph& g, Editor::Node& n); // run a ScriptStart chain
    bool StartHook(Editor::Graph& g, Editor::Node& n);     // register this Pre/PostHook node
    void StopHook(Editor::Node& n);                        // unregister
    void TickFrame(Editor::Graph& g);  // fire OnFrameRender chains (per frame)
}
