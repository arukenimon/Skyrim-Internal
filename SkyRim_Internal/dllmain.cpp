// dllmain.cpp : Minimal DLL thread and unload template.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <iostream>

#include "DirectXOverlay.hpp"

namespace
{
    void OpenConsole(FILE*& consoleOutput)
    {
        if (!AllocConsole())
            return;

        freopen_s(&consoleOutput, "CONOUT$", "w", stdout);
    }

    void CloseConsole(FILE* consoleOutput)
    {
        if (consoleOutput)
            fclose(consoleOutput);

        FreeConsole();
    }
}

DWORD __stdcall EjectThread(LPVOID parameter)
{
    // Let Menu return before the module is unloaded.
    Sleep(100);
    FreeLibraryAndExitThread(static_cast<HMODULE>(parameter), 0);
}

DWORD WINAPI Menue(LPVOID parameter)
{
    const HMODULE hModule = static_cast<HMODULE>(parameter);
    FILE* consoleOutput = nullptr;
    OpenConsole(consoleOutput);

#ifdef _DEBUG
    std::cout << "Build: Debug x64.\n";
#else
    std::cout << "Build: Release x64.\n";
#endif

    const bool overlayInstalled = Skyrim::Overlay::Install();
    std::cout << "DX11 skeleton ESP: " << (overlayInstalled ? "ready" : "hook install failed") << ".\n";
    std::cout << "Scanning nearby actors, including the player in third person.\n";
    std::cout << "Dead actors hidden. Green: player teammates. Blue: you. Cyan: other NPCs (not necessarily enemies).\n";
    std::cout << "F6: bone indices on/off. F7: skeleton on/off. Default: skeleton only.\n";
    std::cout << "Press F9 to restart actor discovery.\n";
    std::cout << "Press F10 to toggle ESP CPU timing reports.\n";
    std::cout << "Press Insert to toggle drawing. Press Numpad 0 to unload.\n";

    while ((GetAsyncKeyState(VK_NUMPAD0) & 1) == 0)
        Sleep(50);

    Skyrim::Overlay::Shutdown();
    std::cout << "Unloading DLL.\n";
    CloseConsole(consoleOutput);

    const HANDLE ejectThread = CreateThread(nullptr, 0, EjectThread, hModule, 0, nullptr);
    if (ejectThread)
        CloseHandle(ejectThread);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID)
{
    if (reasonForCall == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        const HANDLE menuThread = CreateThread(nullptr, 0, Menue, hModule, 0, nullptr);
        if (menuThread)
            CloseHandle(menuThread);
    }

    return TRUE;
}
