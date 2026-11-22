<p align="center"><img src="docs/logo.png" width="96"></p>

# PLAYERSLICE

Player de vídeo para **ecrãs LED fora de formato** — vários ledwalls com tamanhos e
formatos diferentes, alimentados pelas saídas de uma (ou mais) placas gráficas.
Na linha do Resolume Arena / Pixera / Millumin, mas focado numa coisa: pôr cada
vídeo no sítio certo de forma rápida, prática e estável.

Criado por **Daniel Nunes** — [github.com/NunesDanielPT](https://github.com/NunesDanielPT)

---

## Como funciona

```
 Clips (layers × colunas)  ──►  Composição (ex. 7680x2160)  ──►  Slices  ──►  Saídas da gráfica
                                  cada layer pode ter uma          recortam a        monitores / projetores /
                                  "Região" (a zona do ledwall)     composição ou     processadores LED
                                                                   uma layer só
```

* **Composição** — uma tela grande onde se arrumam todos os conteúdos.
* **Layers** — linhas da grelha de clips. Cada layer pode ter uma **região** na
  composição (ex. a zona do ledwall `CENTER T`, 720x648): os vídeos dessa layer
  encaixam lá automaticamente (esticar / caber / preencher / tamanho nativo).
* **Colunas** — clicar no cabeçalho de uma coluna dispara todas as layers ao mesmo tempo.
* **Ecrãs** — cada ecrã é uma saída: um monitor/saída física da gráfica, uma
  janela (para testar), ou desligado.
* **Slices** — cada slice recorta um retângulo da composição (**Entrada**) e
  coloca-o numa posição/tamanho/rotação do ecrã (**Saída**). A fonte pode ser a
  composição inteira ou **só uma layer** (dedicar layers a uma saída).

Fluxo típico: o cliente entrega cada vídeo no formato exato de cada ledwall →
crias uma slice por ledwall → **"Criar layer dedicada a esta slice"** → arrastas
os vídeos do Explorador para essa layer.

## Editor de Saídas e Slices

| Ação | Como |
|---|---|
| Mover | arrastar a slice (Shift = só um eixo) |
| Redimensionar | 8 pegas (Shift = manter proporção, Ctrl = a partir do centro) |
| Rodar | círculo turquesa por cima da slice (Shift = passos de 15°) |
| Snap a bordas e centros | automático (Alt desliga) |
| Afinar ao píxel | setas (Shift = 10 px) |
| Duplicar / Apagar | Ctrl+D / Del |
| Zoom / mover vista | roda do rato / botão do meio ou Espaço + arrastar |
| Desfazer / Refazer | Ctrl+Z / Ctrl+Y |

O fundo do editor mostra o vídeo real (composição na Entrada, saída na Saída) e o
**Test pattern** desenha xadrez, círculo e diagonais em cada slice, na cor da slice.

## Motor

* **C++20 + Direct3D 11**, uma thread de descodificação por clip, filas de frames,
  a thread de render nunca espera pelo disco.
* **HAP / HAP Alpha / HAP Q / HAP Q Alpha / HAP R** — descompressão Snappy em
  paralelo e envio dos blocos BC diretamente para a GPU (o mais leve possível).
* **H.264 / H.265 / VP9 / AV1** — descodificação por hardware (NVDEC via
  D3D11VA) partilhando o dispositivo D3D11: o frame nunca passa pela CPU.
* **ProRes / DNxHR / outros** — descodificação multithread pela CPU.
* Arranque sem salto (o relógio do clip arranca no primeiro frame), troca de clip
  sem frame preto, crossfade por layer.
* Amostragem píxel-a-píxel automática quando o vídeo/slice tem o tamanho exato.
* Saídas em janelas sem bordas por monitor, sem cursor, sem roubar o foco;
  a primeira saída marca o vsync.

## Estabilidade

* Erros num vídeo marcam só esse clip como **ERRO** — nunca derrubam o programa.
* Proteção contra crashes: grava minidump + **projeto de recuperação**, que é
  proposto no arranque seguinte.
* Gravação automática a cada 30 s, gravação atómica dos projetos (nunca fica um ficheiro meio escrito).
* Prioridade alta, timer de 1 ms, Windows impedido de desligar ecrãs/entrar em suspensão.
* Uma só instância de cada vez.
* Registos em `%LOCALAPPDATA%\PLAYERSLICE\` (menu Ficheiro → Abrir pasta de registos).

## Recomendações de codec

| Uso | Codec |
|---|---|
| Ledwalls grandes / muitas layers / 8K | **HAP** ou **HAP Q** (qualidade) |
| Com transparência | HAP Alpha / HAP Q Alpha |
| Ficheiros pequenos, 1–4 vídeos ao mesmo tempo | H.264 / H.265 (GPU) |
| Masters de edição | ProRes (CPU — mais pesado) |

## Compilar

Os builds oficiais são feitos automaticamente pelo **GitHub Actions** em cada
push (separador *Actions* → artefacto `PLAYERSLICE-win64` com `.zip` e instalador).
Uma tag `v*` cria uma *release*.

Localmente (Visual Studio 2022 ou 2026 + CMake):

```powershell
./scripts/get-deps.ps1
cmake -S . -B build -A x64
cmake --build build --config Release
```

Requisitos para correr: Windows 10/11 64-bit, placa gráfica com Direct3D 11
(recomendado NVIDIA RTX).

## Licenças de terceiros

FFmpeg (LGPL, build shared da BtbN), Dear ImGui (MIT), HAP (BSD, Vidvox),
Snappy (BSD, Google), nlohmann/json (MIT).
