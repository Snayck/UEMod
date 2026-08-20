#include <windows.h>
#include "app.h"

// Injected DLL entry. Spins the editor overlay up on its own thread so we never
// block or run inside the loader lock.
static DWORD WINAPI EditorThread(LPVOID)
{
    RunApp();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, EditorThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
