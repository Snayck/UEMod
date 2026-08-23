#pragma once
#include "graph.h"

namespace CallLogger {
    void Init();          // attach the call sink (no-op standalone)
    void Shutdown();
    void Render();        // dockable tab next to the Node Editor
    void FlushSpawns(Editor::Graph& g);   // consume right-click spawn requests
}
