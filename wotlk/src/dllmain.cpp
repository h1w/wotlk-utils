// dllmain.cpp : Defines the entry point for the DLL application.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "logging/logger_setup.hpp"
#include "hooks/hooks.h"
#include "warden/shadow_copy.h"
#include "warden/mpq_cache.h"
#include "warden/peb_unlink.h"
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

    if (shadow::Initialize()) {
        LOG(INFO) << "Shadow copy initialized";
    } else {
        LOG(WARNING) << "Shadow copy initialization failed (non-fatal)";
    }

    // Load MPQ hash cache — must be before hooks (SpoofCmsgIfNeeded calls LookupHash)
    // and before PEB unlinking (uses GetModuleFileName).
    if (mpq_cache::Initialize(hModule)) {
        LOG(INFO) << "MPQ hash cache initialized";
    } else {
        LOG(WARNING) << "MPQ hash cache initialization failed (non-fatal)";
    }

    if (hooks::Initialize()) {
        LOG(INFO) << "Hooks initialized successfully";
    } else {
        LOG(ERROR) << "Failed to initialize hooks";
    }

    // Hide our DLL + dependencies from PEB.Ldr (prevents Warden MODULE_CHECK detection).
    // Must be AFTER all init that uses GetModuleHandle/GetModuleFileName.
    peb_unlink::UnlinkAll(hModule);

    // Ожидание сигнала выгрузки
    HANDLE hEvent = CreateEventA(nullptr, TRUE, FALSE, kUnloadEventName);
    if (hEvent)
    {
        WaitForSingleObject(hEvent, INFINITE);
        CloseHandle(hEvent);
    }

    // Cleanup — безопасно, т.к. мы НЕ в DllMain
    LOG(INFO) << "Unloading wotlk DLL...";

    // Restore PEB entries before unload — LdrUnloadDll needs them to find the module.
    peb_unlink::RelinkAll();

    // Save captured MPQ hashes before shutdown
    mpq_cache::Shutdown();

    // Даём время завершиться вызовам хуков, которые могут быть in-flight
    // в основном потоке игры (FrameScript_Execute вызывается из main thread)
    Sleep(200);
    hooks::Shutdown();
    shadow::Shutdown();
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
        if (HANDLE hThread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr))
            CloseHandle(hThread);
        break;
    case DLL_PROCESS_DETACH:
        // Cleanup уже сделан в MainThread перед FreeLibraryAndExitThread
        break;
    }
    return TRUE;
}
