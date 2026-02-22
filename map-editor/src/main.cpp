#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "app.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    mapedit::App app;
    if (!app.Initialize(hInstance))
        return 1;

    app.Run();
    app.Shutdown();
    return 0;
}
