// PLAYERSLICE — player de vídeo para ecrãs LED fora de formato
// Criado por Daniel Nunes — github.com/NunesDanielPT
#include "app/App.h"
#include <windows.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // coordenadas em píxeis reais em todos os monitores (essencial para as saídas)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // impedir duas instâncias a lutar pelas mesmas saídas
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Global\\PLAYERSLICE_SINGLE_INSTANCE");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"O PLAYERSLICE já está aberto.", L"PLAYERSLICE", MB_ICONINFORMATION);
        return 0;
    }
    static ps::App app;
    int r = app.run(inst);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    CoUninitialize();
    return r;
}
