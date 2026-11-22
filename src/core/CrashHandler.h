#pragma once
#include <string>

namespace ps {
struct CrashHandler {
    using SaveFn = void (*)();
    static void install(const std::wstring& dir, SaveFn emergencySave);
};
}
