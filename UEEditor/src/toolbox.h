#pragma once
#include "graph.h"
#include <string>

// limited=true hides events (used inside the custom-node body editor)
namespace Toolbox {
    void Render(Editor::Graph& g, bool limited = false);
}
