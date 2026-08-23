#pragma once

// Creates the overlay window, runs the ImGui + node-editor loop until closed.
// Shared by the standalone .exe (main.cpp) and the injected DLL (dllmain.cpp).
int RunApp();

// Ask the RunApp loop to exit at the next frame (callable from any thread,
// including DllMain). Safe if the loop isn't running yet/anymore.
void RequestAppExit();
