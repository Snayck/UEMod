#pragma once

// Creates the overlay window, runs the ImGui + node-editor loop until closed.
// Shared by the standalone .exe (main.cpp) and the injected DLL (dllmain.cpp).
int RunApp();
