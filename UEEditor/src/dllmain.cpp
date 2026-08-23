#include <windows.h>
#include "app.h"

static HMODULE g_Module = nullptr;
static HANDLE  g_Thread = nullptr;
static DWORD   g_ThreadId = 0;

extern "C" __declspec(dllexport) void UEEditorRequestUnload() {
    RequestAppExit();
}

static DWORD WINAPI EditorThread(LPVOID) {
    RunApp();
    Sleep(250);   // let any game thread still inside the ProcessEvent detour drain
    FreeLibraryAndExitThread(g_Module, 0);
}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_Module = hModule;
        g_Thread = CreateThread(nullptr, 0, EditorThread, nullptr, 0, &g_ThreadId);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (GetCurrentThreadId() == g_ThreadId)
            return TRUE;
        if (lpReserved != nullptr)
            return TRUE;
        RequestAppExit();
        if (g_Thread)
            WaitForSingleObject(g_Thread, 10000);
    }
    return TRUE;
}
