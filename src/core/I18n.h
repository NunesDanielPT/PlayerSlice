#pragma once
#include <string>
#include <vector>

// Traduções: o texto em português é a chave; T() devolve a versão no idioma ativo.
// Partes depois de "##" (IDs do ImGui) são preservadas sem tradução.
// REGRA: cada texto novo na interface tem de ter entrada em I18n.cpp (EN).
namespace ps {

enum class Lang { PT = 0, EN = 1 };

void        setLanguage(Lang l);
Lang        language();
const char* T(const char* pt);
std::string langCode(Lang l);       // "pt" / "en"
Lang        langFromCode(const std::string& c);

// lista de chaves sem tradução EN (usado em testes)
std::vector<std::string> missingTranslations(const std::vector<std::string>& keys);

} // namespace ps
