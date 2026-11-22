# Dicionário PT -> EN do PLAYERSLICE. Gera src/core/I18nStrings.inc
# Correr:  python scripts/i18n_en.py   (falha se alguma frase da interface não tiver tradução)
EN = {
'Projeto PLAYERSLICE':'PLAYERSLICE project','Vídeo':'Video','Todos':'All',
'Gravar alterações ao projeto antes de sair?':'Save changes to the project before quitting?',
'Erro gráfico: ':'Graphics error: ','Não foi possível criar a janela principal':'Could not create the main window',
'O PLAYERSLICE fechou inesperadamente.\\nRecuperar o projeto?':'PLAYERSLICE closed unexpectedly.\\nRecover the project?',
'Monitor':'Monitor','  (principal)':'  (main)',
'Parar layer':'Stop layer','Bypass (esconder)':'Bypass (hide)','Solo':'Solo','%s  ·  fade %.1fs':'%s  ·  fade %.1fs',
'Adicionar layer acima':'Add layer above','Duplicar layer':'Duplicate layer',' (cópia)':' (copy)',
'Mover para cima':'Move up','Mover para baixo':'Move down','Esvaziar clips':'Clear clips','Apagar layer':'Delete layer',
'ficheiro inválido':'invalid file','ERRO':'ERROR','Escolher vídeo(s)...':'Choose video(s)...','Tocar':'Play',
'Remover clip':'Remove clip','+ Layer':'+ Layer','+ Coluna':'+ Column','Abrir vídeos...':'Open videos...',
'Parar tudo':'Stop all','  Arrasta vídeos do Explorador para as células · Ctrl+arrastar copia':'  Drag videos from Explorer onto the cells · Ctrl+drag copies',
'Composição %dx%d':'Composition %dx%d','Nome':'Name','Apagar coluna':'Delete column','Clips':'Clips',
'Não foi possível gravar as definições':'Could not save the settings','Idioma':'Language','Saídas':'Outputs',
'Quando uma saída é desligada:':'When an output is turned off:',
'Mostrar preto (recomendado em espetáculo)':'Show black (recommended during a show)',
'Fechar a janela (vê-se o ambiente de trabalho)':'Close the window (the desktop becomes visible)',
'Ligar as saídas automaticamente ao abrir um projeto':'Turn outputs on automatically when opening a project',
'Placa gráfica':'Graphics card',
'Placa usada para descodificar e desenhar os vídeos. As saídas ligadas a outra placa também funcionam, mas com mais carga.':'Card used to decode and render the videos. Outputs connected to another card also work, but with more load.',
'Em uso:':'In use:','Automática (a mais potente)':'Automatic (the most powerful)',
'Reinicia o PLAYERSLICE para mudar de placa.':'Restart PLAYERSLICE to switch cards.','Placa de rede':'Network card',
'Rede usada para controlo e vídeo por rede (OSC, Art-Net, NDI). Estas funções estão em desenvolvimento; a escolha fica já guardada.':'Network used for control and network video (OSC, Art-Net, NDI). These features are in development; your choice is already saved.',
'Todas as placas':'All cards','sem IP':'no IP','desligada':'disconnected','Placa de som':'Sound card',
'Saída de áudio dos clips. A reprodução de som está em desenvolvimento; a escolha fica já guardada.':'Audio output for the clips. Sound playback is in development; your choice is already saved.',
'Predefinida do Windows':'Windows default','predefinida':'default','Atalhos de teclado':'Keyboard shortcuts',
'Clica num atalho e carrega na nova tecla. Esc cancela, Backspace apaga.':'Click a shortcut and press the new key. Esc cancels, Backspace clears.',
'Ação':'Action','Atalho':'Shortcut','carrega numa tecla...':'press a key...','Este atalho está repetido':'This shortcut is used twice',
'Repor atalhos de origem':'Restore default shortcuts','Atualizações':'Updates','Versão instalada:':'Installed version:',
'Procurar atualizações ao abrir o PLAYERSLICE':'Check for updates when PLAYERSLICE starts','Sobre':'About',
'Player de vídeo para ecrãs LED fora de formato.':'Video player for non-standard LED screens.',
'Software criado por':'Software created by','Sistema':'System','Placa gráfica:':'Graphics card:','Pasta de dados:':'Data folder:',
'Abrir pasta de registos':'Open log folder','Licenças':'Licenses',
'Usa FFmpeg (LGPL), Dear ImGui (MIT), HAP (BSD, Vidvox), Snappy (BSD), nlohmann/json (MIT).':'Uses FFmpeg (LGPL), Dear ImGui (MIT), HAP (BSD, Vidvox), Snappy (BSD), nlohmann/json (MIT).',
'Tens a versão mais recente.':'You have the latest version.','Não foi possível verificar:':'Could not check:',
'Procurar atualizações':'Check for updates','A procurar atualizações...':'Checking for updates...',
'Nova versão disponível:':'New version available:','Atualizar agora':'Update now','A descarregar...':'Downloading...',
'Pronto a instalar. O PLAYERSLICE vai fechar e voltar a abrir.':'Ready to install. PLAYERSLICE will close and reopen.',
'Instalar e reiniciar':'Install and restart','Há saídas no ar. Instalar mesmo assim?':'Outputs are live. Install anyway?',
'Mais tarde':'Later','Definições':'Settings','Atualizar listas':'Refresh lists','Atualização disponível':'Update available',
'+ Ecrã':'+ Screen','Saída ':'Output ','  [janela]':'  [window]','  [desligado]':'  [off]','  ● NO AR':'  ● LIVE',
'Adicionar slice':'Add slice','Slice ':'Slice ','Duplicar ecrã':'Duplicate screen','Apagar ecrã':'Delete screen','+ Slice':'+ Slice',
'Ecrã':'Screen','Desligado':'Off','Janela (teste)':'Window (test)','  (SEM SINAL)':'  (NO SIGNAL)','Saída':'Output',
"Atenção: '%s' é o monitor principal — a saída vai tapar a interface.":"Warning: '%s' is the main monitor — the output will cover the interface.",
'Placa: %s':'Card: %s','Resolução':'Resolution','Brilho':'Brightness','Test pattern':'Test pattern','Saída ligada':'Output on',
'No atalho':'In shortcut','Este ecrã liga/desliga com o atalho das saídas.':'This screen turns on/off with the outputs shortcut.',
'Ajustar resolução às slices':'Fit resolution to slices','Slice':'Slice','Composição':'Composition','Fonte':'Source',
'Composição = todas as layers misturadas.\\nLayer = só essa layer (dedicar layers a uma saída).':'Composition = all layers mixed.\\nLayer = only that layer (dedicate layers to an output).',
'Entrada (recorte da composição)':'Input (crop of the composition)','Saída (posição no ecrã)':'Output (position on the screen)',
'Rotação':'Rotation','%.2f°':'%.2f°','Espelhar H':'Flip H','Espelhar V':'Flip V','Opacidade':'Opacity','Cor':'Color','Atalhos':'Shortcuts',
'Saída 1:1 (= tamanho da entrada)':'Output 1:1 (= input size)','Entrada = Saída (mesma posição)':'Input = Output (same position)',
'Criar layer dedicada a esta slice':'Create a layer dedicated to this slice',
'Cria uma layer cuja região é o recorte desta slice.\\nOs vídeos dessa layer encaixam automaticamente aqui.':'Creates a layer whose region is this slice\'s crop.\\nThe videos in that layer fit here automatically.',
'Adicionar slice aqui':'Add slice here','Duplicar':'Duplicate','Rodar 90°':'Rotate 90°','Saída 1:1':'Output 1:1',
'Ocupar o canvas inteiro':'Fill the whole canvas','Apagar':'Delete','X / Y':'X / Y','L / A':'W / H',
'Largura / Altura em píxeis':'Width / Height in pixels','Entrada  (Input Selection)':'Input Selection',
'Saída  (Output Transformation)':'Output Transformation','Ajustar':'Fit','Snap':'Snap','Grelha':'Grid','  %s: %dx%d':'  %s: %dx%d',
'Arrastar: mover   ·   Pegas: redimensionar (Shift = proporção, Ctrl = a partir do centro)':'Drag: move   ·   Handles: resize (Shift = keep ratio, Ctrl = from center)',
'Círculo turquesa: rodar (Shift = 15°)   ·   Alt: desligar snap':'Turquoise circle: rotate (Shift = 15°)   ·   Alt: disable snap',
'Setas: 1 px (Shift = 10 px)   ·   Del: apagar   ·   Ctrl+D: duplicar':'Arrows: 1 px (Shift = 10 px)   ·   Del: delete   ·   Ctrl+D: duplicate',
'Roda do rato: zoom   ·   Botão do meio ou Espaço+arrastar: mover a vista':'Mouse wheel: zoom   ·   Middle button or Space+drag: pan the view',
'Adiciona um ecrã à esquerda.':'Add a screen on the left.','\\nSeleciona uma slice para editar.':'\\nSelect a slice to edit.',
'Saídas e Slices':'Outputs and Slices','Definições...':'Settings...','SAÍDAS LIGADAS':'OUTPUTS ON','SAÍDAS DESLIGADAS':'OUTPUTS OFF',
'Liga/desliga os ecrãs marcados \\"no atalho\\" (se nenhum estiver marcado, todos).':'Turns on/off the screens marked \\"in shortcut\\" (all of them if none is marked).',
'Ligar todas':'All on','Desligar todas':'All off','Sem ecrãs. Cria um em \\"Saídas e Slices\\".':'No screens. Create one in \\"Outputs and Slices\\".',
'SEM SINAL':'NO SIGNAL','NO AR':'LIVE','Ligada':'On','Seleciona uma layer.':'Select a layer.','Clip':'Clip',
'Atenção: o vídeo não tem o tamanho da região (%.0fx%.0f)':'Warning: the video does not match the region size (%.0fx%.0f)',
'Dica: para vídeos muito grandes usa HAP / HAP Q.':'Tip: for very large videos use HAP / HAP Q.',
'A tocar: %s  ·  fila %d  ·  descartados %d':'Playing: %s  ·  queue %d  ·  dropped %d','Loop':'Loop',
'Tocar uma vez e parar no fim':'Play once and hold the last frame','Tocar uma vez e limpar':'Play once and clear',
'Repetição':'Repeat','Velocidade':'Speed','Esticar':'Stretch','Caber (com barras)':'Fit (letterbox)','Preencher (cortar)':'Fill (crop)',
'Tamanho nativo':'Native size','Encaixe':'Fit mode','Escala':'Scale','Deslocar':'Offset','Repor transformação':'Reset transform',
'Layer':'Layer','Mistura':'Blend','Fade (s)':'Fade (s)','Região na composição':'Region in the composition','Usar região':'Use region',
'Os vídeos desta layer encaixam neste retângulo da composição\\n(ex.: a zona de um ledwall). Desligado = composição inteira.':'Videos in this layer fit into this rectangle of the composition\\n(e.g. the area of one LED wall). Off = whole composition.',
'Copiar de':'Copy from','slice...':'slice...','Tamanho = vídeo selecionado':'Size = selected video',
'FPS de referência':'Reference FPS','Máximo da GPU: 16384x16384. VRAM: %zu MB':'GPU maximum: 16384x16384. VRAM: %zu MB',
'Aplicar':'Apply','Cancelar':'Cancel','|  clips a tocar: %d':'|  clips playing: %d','|  frames descartados: %d':'|  dropped frames: %d',
'|  saídas no ar: %d':'|  live outputs: %d','sem título':'untitled','Ficheiro':'File','Novo projeto':'New project',
'Descartar alterações?':'Discard changes?','Abrir...':'Open...','Gravar':'Save','Gravar como...':'Save as...','Sair':'Quit',
'Editar':'Edit','Desfazer':'Undo','Refazer':'Redo','Definições da composição...':'Composition settings...',
'Adicionar layer':'Add layer','Adicionar coluna':'Add column','Ligar/desligar saídas marcadas':'Toggle marked outputs',
'Ligar todas as saídas':'Turn all outputs on','Desligar todas as saídas':'Turn all outputs off',
'Editor de saídas e slices':'Outputs and slices editor','Detetar monitores de novo':'Detect monitors again',
'Saídas detetadas:':'Detected outputs:','Ver':'View','Repor disposição das janelas':'Reset window layout','Ajuda':'Help',
'Sobre o PLAYERSLICE':'About PLAYERSLICE','● NO AR':'● LIVE','○ saídas desligadas':'○ outputs off',
'Uma saída está no mesmo monitor que a interface.':'An output is on the same monitor as the interface.',
'Monitor de saídas':'Output monitor','Propriedades':'Properties','Definições da composição':'Composition settings','Registo':'Log',
'Parar todos os clips':'Stop all clips','Test pattern (ecrã selecionado)':'Test pattern (selected screen)','Gravar como':'Save as',
'Abrir projeto':'Open project','Geral':'General',
'Enviar preview para programa':'Send preview to program','Limpar preview':'Clear preview',
'▲ PROGRAMA':'▲ PROGRAM','▼ PREVIEW':'▼ PREVIEW','Pôr no programa':'Put on program','Pôr em preview':'Put in preview',
'  Clique: metade de cima = programa · metade de baixo = preview · cabeçalho = coluna inteira no ar':'  Click: top half = program · bottom half = preview · header = whole column live',
'Programa (no ar)':'Program (live)','Preview':'Preview','Nenhum':'None','Fundo:':'Background:',
'Opacidade do vídeo de fundo':'Background video opacity','Ecrã: ':'Screen: ','Slice: ':'Slice: ',
'Ecrãs (como saem nas saídas)':'Screens (as sent to the outputs)','Slices (só o recorte)':'Slices (crop only)',
'PREVIEW':'PREVIEW','PROGRAMA':'PROGRAM','Enviar para programa':'Send to program','Limpar':'Clear',
'%d layer(s) no ar  ·  %dx%d':'%d layer(s) live  ·  %dx%d','Preview / Programa':'Preview / Program',
**{f'Disparar coluna {i}':f'Trigger column {i}' for i in range(1,10)},
}

if __name__ == '__main__':
    import json, os, re, glob, sys
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    keys = []
    def add(k):
        k = k.split('##')[0]
        if k and k not in keys: keys.append(k)
    for f in sorted(glob.glob(os.path.join(root, 'src', '**', '*.cpp'), recursive=True)):
        s = open(f, encoding='utf-8').read()
        for m in re.finditer(r'\bT\("((?:[^"\\]|\\.)*)"\)', s): add(m.group(1))
        for m in re.finditer(r'winTitle\("((?:[^"\\]|\\.)*)"', s): add(m.group(1))
        for name in ('kActionNames[] = {', 'const char* tabs[] = {'):
            if name in s:
                i = s.index(name); blk = s[i:s.index('};', i)]
                for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', blk): add(m.group(1))
    missing = [k for k in keys if k not in EN]
    if missing:
        print('SEM TRADUÇÃO EN:'); [print('  ', repr(k)) for k in missing]; sys.exit(1)
    out = os.path.join(root, 'src', 'core', 'I18nStrings.inc')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('// GERADO por scripts/i18n_en.py — não editar à mão\n')
        for k in keys: f.write('{"%s", "%s"},\n' % (k, EN[k]))
    print(f'{len(keys)} frases traduzidas -> {out}')
