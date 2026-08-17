#include "toolbox.h"
#include "nodes.h"

using namespace Toolbox;
using namespace Nodes;

static void Entry(const char* label, Node* (*spawn)(Graph&), Graph& g) {
    if (ImGui::Button(label, ImVec2(-1, 0)))
        spawn(g);
}

void Toolbox::Render(Editor::Graph& g) {
    if (ImGui::CollapsingHeader("Events", ImGuiTreeNodeFlags_DefaultOpen)) {
        Entry("On Initialize", SpawnInitScriptNode, g);
        Entry("On Key Press",  OnKeyPress, g);
        Entry("On Frame Render", OnFrameRender, g);
        Entry("Pre Hook",  PreHook, g);
        Entry("Post Hook", PostHook, g);
        Entry("Get Param", GetParam, g);
    }
    if (ImGui::CollapsingHeader("Flow", ImGuiTreeNodeFlags_DefaultOpen)) {
        Entry("Sequence", Sequence, g);
        Entry("Branch",   Branch, g);
        Entry("For Each", ForEach, g);
        Entry("For Loop", ForLoop, g);
        Entry("While",    WhileLoop, g);
    }
    if (ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
        Entry("Get World",           GetWorld, g);
        Entry("Find Object",         FindObject, g);
        Entry("Get Objects Of Class", GetObjectsOfClass, g);
        Entry("Get Value",           GetValue, g);
        Entry("Set Value",           SetValue, g);
        Entry("Call Function",       CallFunction, g);
        Entry("Is Valid",            IsValid, g);
        Entry("Is A",                IsA, g);
        Entry("Get Name",            GetName, g);
        Entry("Get Class Name",      GetClassName, g);
        Entry("Get Full Name",       GetFullName, g);
        Entry("Get All Fields",      GetAllFields, g);
        Entry("Get All Methods",     GetAllMethods, g);
        Entry("Array Length",        ArrayLength, g);
        Entry("Array Get",           ArrayGet, g);
    }
    if (ImGui::CollapsingHeader("Values")) {
        Entry("Literal", Literal, g);
    }
    if (ImGui::CollapsingHeader("Math")) {
        Entry("Add",      Add, g);
        Entry("Subtract", Subtract, g);
        Entry("Multiply", Multiply, g);
        Entry("Divide",   Divide, g);
        Entry("A > B",    Greater, g);
        Entry("A < B",    Less, g);
        Entry("A == B",   Equal, g);
        Entry("And",      And, g);
        Entry("Or",       Or, g);
        Entry("Not",      Not, g);
    }
    if (ImGui::CollapsingHeader("Draw")) {
        Entry("Draw Text", DrawText, g);
        Entry("Draw Line", DrawLine, g);
    }
    if (ImGui::CollapsingHeader("Debug")) {
        Entry("Print", Print, g);
    }
}
