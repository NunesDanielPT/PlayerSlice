// Proteção contra crashes: grava um minidump + força gravação de emergência do
// projeto, para que no próximo arranque o trabalho seja recuperado.
#include "core/CrashHandler.h"
#include "core/Log.h"
#include <windows.h>
#include <dbghelp.h>
#include <atomic>

namespace ps {

static std::wstring           g_dumpDir;
static CrashHandler::SaveFn   g_emergencySave;
static std::atomic<bool>      g_inCrash{false};

static LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    if (g_inCrash.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[256];
    swprintf(name, 256, L"%s\\crash_%04d%02d%02d_%02d%02d%02d.dmp", g_dumpDir.c_str(),
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                          &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    PS_ERROR("CRASH 0x%08lX — minidump gravado", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
    if (g_emergencySave) {
        try { g_emergencySave(); } catch (...) {}
    }
    Log::shutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

void CrashHandler::install(const std::wstring& dir, SaveFn emergencySave) {
    g_dumpDir = dir + L"\\crashes";
    CreateDirectoryW(g_dumpDir.c_str(), nullptr);
    g_emergencySave = emergencySave;
    SetUnhandledExceptionFilter(onUnhandled);
    // não mostrar caixas de erro do Windows a meio de um espetáculo
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

} // namespace ps
