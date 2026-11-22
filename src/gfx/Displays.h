#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace ps {

// Uma saída física da gráfica (monitor, projetor, processador LED, ...)
struct DisplayInfo {
    std::string key;        // \\.\DISPLAY2 (identificador estável enquanto a ligação não muda)
    std::string friendly;   // nome do monitor (EDID), ex. "OMEN 27"
    std::string adapter;    // nome da gráfica
    RECT        rect{};     // posição no ambiente de trabalho
    int         refreshHz = 0;
    bool        primary = false;
    HMONITOR    monitor = nullptr;

    int width() const  { return rect.right - rect.left; }
    int height() const { return rect.bottom - rect.top; }
    std::string label() const;
};

std::vector<DisplayInfo> enumerateDisplays();
// Põe o monitor (\\.\DISPLAYn) à frequência mais próxima de fps (mesma resolução).
// Fica gravado no Windows. Devolve a frequência final (0 = falhou / sem modo compatível).
int setDisplayRefresh(const std::string& key, float fps, std::string* msg = nullptr);
int currentRefresh(const std::string& key);

} // namespace ps
