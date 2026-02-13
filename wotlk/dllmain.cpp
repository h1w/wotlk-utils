// dllmain.cpp : Defines the entry point for the DLL application.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "logging/logger_setup.hpp"
#include <glog/logging.h>

#include <cstdio>

// Named event для сигнала выгрузки из инжектора
static const char* kUnloadEventName = "wotlk_unload_event";

DWORD WINAPI MainThread(LPVOID lpParam)
{
    HMODULE hModule = (HMODULE)lpParam;

    // открытие консольного окна для инжектированной DLL
    AllocConsole();

    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    // Включить поддержку ANSI escape-кодов (цвета) в консоли
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    hOut = GetStdHandle(STD_ERROR_HANDLE);
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    logger::Initialize({"wotlk", "wotlk", "./logs", true, true});

    LOG(INFO) << "wotlk DLL loaded successfully";

    // Ожидание сигнала выгрузки
    HANDLE hEvent = CreateEventA(nullptr, TRUE, FALSE, kUnloadEventName);
    if (hEvent)
    {
        WaitForSingleObject(hEvent, INFINITE);
        CloseHandle(hEvent);
    }

    // Cleanup — безопасно, т.к. мы НЕ в DllMain
    LOG(INFO) << "Unloading wotlk DLL...";
    logger::Shutdown();

    // Закрыть file handle-ы от freopen_s, иначе консоль не закроется
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    FreeConsole();

    // Атомарно освободить DLL и завершить поток — безопасная самовыгрузка
    FreeLibraryAndExitThread(hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        // Cleanup уже сделан в MainThread перед FreeLibraryAndExitThread
        break;
    }
    return TRUE;
}
