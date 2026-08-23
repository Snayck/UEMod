#pragma once
#include "graph.h"

namespace ObjectExplorer {
    void Render();
    void FlushSpawns(Editor::Graph& g);   // consume right-click spawn requests
}
