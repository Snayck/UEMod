#pragma once
#include "graph.h"
#include <string>
#include <vector>

namespace IO {
    std::string ModuleDir();         // directory of the exe/dll
    std::string BaseDir();           // "<module dir>/UEEditorFiles", created on demand
    std::string ScriptsDir();

    bool SaveGraph(const Editor::Graph& g, const std::string& path);
    bool LoadGraph(Editor::Graph& g, const std::string& path);   // replaces contents

    bool SaveCustomNodes();         // ScriptsDir()/custom_nodes.json
    void LoadCustomNodes();
}
