#pragma once
#include "graph.h"

// Execution layer; no-ops in the standalone .exe (UEEDITOR_WITH_BACKEND).
namespace Exec {
    void InitBackend();
    bool BackendReady();
    const char* BackendStatus();
    void RunScriptNode(Editor::Graph& g, Editor::Node& n); // run a ScriptStart chain
    void RunGraph(Editor::Graph& g);   // register PreHook/PostHook nodes
    void TickFrame(Editor::Graph& g);  // fire OnFrameRender chains (per frame)
}
