#include "toolbox.h"
#include "nodes.h"

using namespace Toolbox;
using namespace Nodes;

void Toolbox::Render(Editor::Graph& g) {
    if (ImGui::Button("GetObjectsOfClass")) GetObjectsOfClass(g);
    if (ImGui::Button("ForEach")) ForEach(g);
    if (ImGui::Button("SetField")) SetField(g);
}