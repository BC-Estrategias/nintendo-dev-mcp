/* Two languages, no library. t(key, ...args) replaces {0}, {1}... ; a missing key returns the key itself (a test makes
 * sure every key used by the page exists in both dictionaries). */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});

  const PT = {
    appName: "Nintendo Dev Agent", files: "Arquivos", settings: "Configurações", faq: "FAQ",
    // generic
    ok: "OK", cancel: "Cancelar", close: "Fechar", open: "Abrir", view: "Ver", save: "Salvar", copy: "Copiar", copied: "Copiado.", copyFailed: "Não consegui copiar.",
    refresh: "Atualizar", loading: "Carregando…", name: "Nome", size: "Tamanho", type: "Tipo", folder: "Pasta", file: "Arquivo", up: "Subir uma pasta",
    download: "Baixar", downloading: "Baixando {0}…", downloaded: "{0} baixado.", upload: "Enviar", rename: "Renomear", renamed: "Renomeado.", restore: "Restaurar",
    filter: "Filtrar…", copyPath: "Copiar caminho", itemsCount: "{0} item(ns)", selectedN: "{0} selecionado(s)", emptyFolder: "Pasta vazia.", noMatches: "Nada encontrado.",
    dropHint: "Arraste arquivos ou pastas para cá para enviar.", dropTo: "Solte para enviar para {0}",
    newFolder: "Nova pasta", folderName: "Nome da pasta", newFolderDefault: "Nova pasta", creatingFolder: "Criando a pasta (o console leva ~6 s)…",
    newFile: "Novo arquivo", fileName: "Nome do arquivo", newName: "Novo nome",
    moveToTrash: "Mover para a lixeira", confirmTrash1: "Mover “{0}” para a lixeira?", confirmTrashN: "Mover {0} itens para a lixeira?",
    trashNote: "Nada é apagado de verdade: fica na pasta .ndp-trash e dá para restaurar.", movedToTrash: "{0} item(ns) na lixeira.", moved: "{0} item(ns) movido(s).", restoredTo: "Restaurado em {0}",
    trashBanner: "Você está na lixeira. Use “Restaurar” (botão direito) para devolver um item ao lugar de origem.",
    // banners
    readOnlyBanner: "O console está em modo SOMENTE LEITURA.", readOnlyHow: "Para enviar ou alterar arquivos, aperte X no console (modo DEVELOPMENT). A página não consegue mudar isso: de propósito, só quem está com o console decide.",
    folderReadOnly: "Esta pasta é só para leitura: o console não liberou escrita aqui. Para mudar, aperte A no console (Access folders).",
    notWritableHere: "Não dá para escrever nesta pasta (o console não liberou ou é zona protegida).",
    notOpenedTitle: "Esta pasta não está liberada", notOpenedBody: "O dono do console precisa liberá-la: aperte A no console, escolha a pasta e use Y para mudar o nível.",
    cannotList: "Não consegui listar esta pasta",
    // uploads
    uploadingN: "Enviando ({0})", uploadsDone: "Envios concluídos", clearFinished: "Limpar concluídos", queued: "Na fila", hashing: "Verificando o arquivo…", uploaded: "Enviado", skipped: "Ignorado", canceled: "Cancelado", failed: "Falhou",
    alreadyExists: "Já existe um arquivo com esse nome", existsBody: "“{0}” já existe nesta pasta. O que fazer?", applyToAll: "Aplicar a todos os próximos", skip: "Ignorar", keepBoth: "Manter os dois", replace: "Substituir", replaceKeepBackup: "Substituir e guardar cópia (.bak)",
    uploadFolders: "Enviar pastas", uploadFoldersNote: "Serão criadas {0} pasta(s) no console (cada uma leva ~6 s). Continuar?",
    uploadFinished: "{0} arquivo(s) enviado(s).", uploadFinishedErrors: "{0} enviado(s), {1} com erro.",
    // viewer / editor
    cannotShowImage: "Não consegui mostrar esta imagem.", binaryFile: "Arquivo binário: mostrando os primeiros bytes em hexadecimal. Use Baixar para obter o arquivo.",
    tooBigToEdit: "Arquivo grande demais para editar aqui ({0}). Mostrando só o início ({1}). Use Baixar.", lines: "linhas", modified: "modificado", readOnlyFile: "somente leitura",
    readOnlyFileHow: "Você pode ler, mas não salvar: o console não está liberando escrita neste arquivo (modo somente leitura, pasta só de leitura ou zona protegida).",
    keepBackup: "Guardar a versão anterior (.bak)", saved: "{0} salvo.", backupKept: "Versão anterior em {0}.", saveFailed: "Não salvou",
    unsavedTitle: "Alterações não salvas", unsavedBody: "Se fechar agora, as alterações deste arquivo serão perdidas.", discard: "Descartar",
    // nome inválido
    "name.empty": "Digite um nome.", "name.chars": "O nome não pode ter \\ / : * ? \" < > | nem caracteres de controle.", "name.trailing": "O nome não pode terminar com ponto ou espaço.",
    "name.tilde": "Evite “~” seguido de número no nome (o cartão SD confunde com nomes curtos).", "name.long": "Nome longo demais (máximo 255 bytes).",
    // connection
    connecting: "Conectando ao console…", cannotReach: "Não consegui falar com o console", cannotReachBody: "Confira se o app Nintendo Dev Agent está aberto no 3DS, se o computador está na mesma rede Wi-Fi e se este endereço é o que aparece na tela de cima do console (o IP muda quando o console reconecta).",
    retryNow: "Tentar agora", connectionLost: "Conexão perdida.", retryingIn: "Nova tentativa em {0} s.",
    "state.connecting": "Conectando", "state.pairing": "Precisa parear", "state.ready": "Conectado", "state.offline": "Sem conexão",
    "mode.READ_ONLY": "Somente leitura", "mode.DEVELOPMENT": "Desenvolvimento",
    // pairing
    pairTitle: "Parear este navegador", pair: "Parear", pairStep1: "No 3DS, aperte Y: o console mostra um código na tela de CIMA (vale por 2 minutos e só funciona uma vez).", pairStep2: "Digite o código abaixo e dê um nome a este navegador.", pairStep3: "Pronto: o console passa a aceitar este navegador.",
    pairCode: "Código do console", pairLabel: "Nome deste navegador", pairLabelHint: "Até 15 caracteres. Aparece só para você lembrar quem pareou.", pairRemember: "Lembrar neste navegador (guarda a chave aqui)", pairBadCode: "Esse código não parece certo: são 16 caracteres, como XXXX-XXXX-XXXX-XXXX.",
    pairFailedHint: "Se o código expirou ou já foi usado, aperte Y de novo no console.", pairRejected: "O console não reconheceu a chave guardada neste navegador (foi esquecida no console?). Pareie de novo.",
    pairWhy: "O código nunca viaja pela rede: só uma prova derivada dele. A chave fica neste navegador e cada pedido depois disso é assinado com ela.", pairWindowClosed: "A janela de pareamento está fechada no console: aperte Y nele para abrir.",
    // settings
    connection: "Conexão", address: "Endereço", status: "Estado", agentVersion: "Versão do agente", platform: "Plataforma", protocol: "Protocolo", mode: "Modo", latency: "Latência", security: "Segurança", authRequired: "Pareamento + assinatura HMAC", authOff: "Desligada (só testes)",
    console: "Console", model: "Modelo", firmware: "Firmware", ram: "RAM", sdCard: "Cartão SD", systemMemory: "Memória do sistema", appMemory: "Memória de aplicativos", usedOf: "{0} de {1} em uso", freeN: "{0} livres", noDeviceInfo: "O console não informou estes dados.", notConnected: "Sem conexão.",
    folders: "Pastas liberadas", foldersNote: "Estas são as pastas que o dono liberou no console. Só dá para mudar no próprio console (botão A): a página só mostra.", levelWrite: "Leitura e escrita", levelRead: "Só leitura",
    noWriteFolders: "Nenhuma pasta com escrita.", noReadFolders: "Nenhuma outra pasta.", protectedNote: "Zonas protegidas (/Nintendo 3DS, /luma, /boot.firm, /gm9, /private) nunca aceitam escrita, mesmo liberadas.",
    pairing: "Pareamento", pairedThis: "Este navegador está pareado como “{0}”.", notPairedThis: "Este navegador não guardou chave (sessão só desta aba).", forgetNote: "Esquecer apaga a chave daqui; o console continua com ela até você apertar SELECT duas vezes nele.", forgetThis: "Esquecer este navegador", forgetThisBody: "A chave será removida deste navegador e você precisará parear de novo.",
    preferences: "Preferências", language: "Idioma", langAuto: "Automático", theme: "Tema", themeAuto: "Automático", themeLight: "Claro", themeDark: "Escuro",
    showHidden: "Mostrar itens ocultos", showHiddenHint: "Nomes que começam com ponto.", confirmDelete: "Confirmar antes de mover para a lixeira", confirmDeleteHint: "Nada é apagado de verdade, mas pergunta mesmo assim.", backupOnSave: "Guardar cópia (.bak) ao salvar por padrão", backupOnSaveHint: "Marcado por padrão no editor.",
    // erros do console
    "err.UNSUPPORTED_PROTOCOL": "Versão de protocolo incompatível.", "err.UNAUTHORIZED": "Não autorizado: pareie este navegador.", "err.FORBIDDEN_MODE": "O console está em somente leitura: aperte X nele.", "err.PROTECTED_PATH": "Pasta não liberada ou protegida: aperte A no console para liberar.",
    "err.NOT_FOUND": "Não encontrado.", "err.EXISTS": "Já existe.", "err.IO_ERROR": "Erro de leitura/escrita no cartão SD.", "err.NO_SPACE": "Sem espaço no cartão SD.", "err.BAD_REQUEST": "Pedido inválido.", "err.BUSY": "O console está ocupado: tente de novo em instantes.",
    "err.TOO_LARGE": "Grande demais.", "err.HASH_MISMATCH": "O conteúdo não confere (SHA-256): nada foi gravado.", "err.TIMEOUT": "O console demorou demais para responder.", "err.UNSUPPORTED_COMMAND": "O app do console é antigo demais para isso: atualize-o.", "err.HELLO_REQUIRED": "Conexão fora de ordem.", "err.BAD_FRAME": "Mensagem inválida.", "err.PATH_INVALID": "Caminho ou nome inválido.",
    // FAQ
    "faq.intro": "Respostas rápidas. Clique numa pergunta para abrir.",
    "faq.what.q": "O que é esta página?",
    "faq.what.a": "É o gerenciador de arquivos do seu 3DS, servido pelo próprio app Nintendo Dev Agent que roda no console. Daqui você navega, vê, edita, envia (arrastando e soltando), baixa, renomeia e apaga arquivos do cartão SD, como um FTP, sem instalar nada.\nSó funciona enquanto o app está aberto no console e o computador está na mesma rede Wi-Fi. O servidor MCP para assistentes de IA continua funcionando ao mesmo tempo, sem mudar nada.",
    "faq.pairing.q": "Como conecto e pareio este navegador?",
    "faq.pairing.a": "O console só atende computadores/navegadores que você autorizou. Na primeira vez: aperte Y no 3DS (o código aparece na tela de cima por 2 minutos) e digite-o na tela de pareamento. A chave fica guardada neste navegador (ou só até fechar a aba, se você desmarcar “Lembrar”).\nPara remover: Configurações → Esquecer este navegador. Para esquecer todos os computadores de uma vez: aperte SELECT duas vezes no console.\nSó um navegador/aba fica conectado por vez: abrir a página em outra aba desconecta a anterior.",
    "faq.keys.q": "O que cada botão faz no console?",
    "faq.keys.a": "- A: abre “Access folders”, onde você escolhe quais pastas a página e o assistente podem ver e alterar (Y muda o nível de uma pasta: nenhum / leitura / escrita).\n- X: alterna SOMENTE LEITURA ⇄ DESENVOLVIMENTO. Ao abrir, o app sempre começa em somente leitura.\n- Y: abre o pareamento (mostra o código).\n- SELECT duas vezes: esquece todos os computadores pareados.\n- START: fecha o app.",
    "faq.modes.q": "Por que não consigo alterar arquivos?",
    "faq.modes.a": "Três travas, todas no console:\n- Modo: em SOMENTE LEITURA você só lê e baixa. Aperte X no console para o modo DESENVOLVIMENTO.\n- Pastas: só se escreve nas pastas liberadas para escrita (padrão: /3ds/nintendo-dev-agent). Mude com o botão A no console; veja a lista em Configurações.\n- Zonas protegidas: /Nintendo 3DS, /luma (exceto plugins e titles), /boot.firm, /gm9 e /private nunca aceitam escrita.\nA página não consegue mudar nada disso, por segurança: quem está com o console decide.",
    "faq.upload.q": "Como envio arquivos? Quais são os limites?",
    "faq.upload.a": "Arraste arquivos ou pastas para qualquer lugar da aba Arquivos (ou para uma pasta da lista), ou use o botão Enviar.\nCada arquivo é verificado aqui, enviado, e o console confere tamanho e SHA-256 antes de colocá-lo no lugar: um envio interrompido não deixa arquivo pela metade. Se o nome já existe, a página pergunta: ignorar, manter os dois, substituir ou substituir guardando uma cópia .bak.\n- A velocidade é a do Wi-Fi do 3DS: cerca de 0,6 a 1 MiB/s. Um arquivo de 100 MiB leva alguns minutos.\n- Um envio por vez, e o console atende uma coisa por vez: mantenha a página aberta.\n- Criar uma pasta leva ~6 s (é assim no console).",
    "faq.edit.q": "Como edito um arquivo?",
    "faq.edit.a": "Dê dois cliques num arquivo de texto (até 1 MiB) para abrir o editor; Ctrl/Cmd+S salva. Marque “Guardar a versão anterior” para manter uma cópia .bak. Imagens abrem numa visualização, arquivos binários mostram os primeiros bytes em hexadecimal, e arquivos grandes só podem ser baixados.\nSalvar é atômico como um envio: se algo falhar, o arquivo original continua intacto. O editor usa UTF-8.",
    "faq.trash.q": "O que acontece quando apago? Dá para recuperar?",
    "faq.trash.a": "Nada é apagado de verdade: o item vai para a pasta .ndp-trash dentro da pasta liberada. Abra a pasta .ndp-trash, clique com o botão direito e escolha Restaurar. A página não esvazia a lixeira; para liberar espaço de verdade, apague pelo computador com o cartão SD.",
    "faq.mcp.q": "Como instalo o MCP local (para Claude Code / Codex)?",
    "faq.mcp.a": "O MCP deixa assistentes de IA usarem o console pelo seu computador. É independente desta página (a página fala direto do navegador com o console; o MCP roda no seu computador) e os dois funcionam juntos.",
    "faq.mcp.s1": "Instale o Node.js 22.18 ou mais novo e confira:", "faq.mcp.s2": "Baixe o projeto e instale as dependências:", "faq.mcp.s3": "Diga onde está a pasta do projeto:", "faq.mcp.where": "Pasta do projeto neste computador", "faq.mcp.s3b": "Os comandos abaixo se ajustam enquanto você digita.",
    "faq.mcp.s4": "Pareie o computador com o console (uma vez). Aperte Y no console e rode, digitando o código da tela:", "faq.mcp.s4b": "É um pareamento separado do deste navegador: cada computador pareia sozinho. {0} é o IP do console agora.",
    "faq.mcp.s5": "Registre o servidor no seu assistente:", "faq.mcp.s5b": "--local-root é a pasta DESTE computador de onde o assistente pode enviar e para onde pode baixar arquivos. Troque pela que preferir.",
    "faq.mcp.s6": "Reinicie a sessão do assistente e peça, por exemplo: “liste os arquivos do meu 3DS”.", "faq.mcp.s6b": "As ferramentas de escrita só funcionam com o console em modo DESENVOLVIMENTO (X). O IP do console muda, mas o MCP o encontra sozinho na rede.",
    "faq.mcp.remove": "Para remover:", "faq.mcp.more": "Detalhes completos em",
    "faq.security.q": "É seguro?",
    "faq.security.a": "A página é servida em HTTP simples na sua rede local. Depois do pareamento, cada mensagem é assinada com HMAC (ninguém forja pedidos), mas o conteúdo dos arquivos NÃO é criptografado: quem escuta seu Wi-Fi consegue ver. Use só em redes de confiança.\nO console recusa pedidos cujo endereço ou origem não batem (protege contra um site malicioso tentar falar com o console pelo seu navegador), não aceita envios por formulário e derruba conexões paradas. Nada sai do console para a internet.\nA chave fica no armazenamento deste navegador: quem usar este perfil do navegador consegue usá-la. Em computador compartilhado, desmarque “Lembrar” ou use “Esquecer este navegador”.",
    "faq.trouble.q": "Deu problema. E agora?",
    "faq.trouble.a": "- Não abre: o app precisa estar aberto no 3DS, o computador na mesma rede Wi-Fi e o endereço igual ao da tela de cima do console (o IP muda quando ele reconecta).\n- “Somente leitura”: aperte X no console.\n- Pasta não liberada (PROTECTED_PATH): aperte A no console e libere a pasta.\n- Ocupado (BUSY): o console faz uma coisa por vez; espere alguns segundos.\n- Lento ou caindo: o Wi-Fi do 3DS dorme quando ocioso e a primeira ação depois de uma pausa demora; a página reconecta sozinha.\n- Janela de pareamento fechada: aperte Y de novo.\n- Console travou com erro “mcu” depois de abrir o menu Rosalina: nos nossos testes isso aconteceu uma vez com o app aberto; feche o app antes de usar o Rosalina e reinicie o console.",
    "faq.about.q": "Sobre",
    "faq.about.a": "Nintendo Dev Agent é software livre (Apache-2.0). Código, versões e problemas: https://github.com/BC-Estrategias/nintendo-dev-mcp\nEsta página e o app do console são um só programa; a versão aparece em Configurações.",
  };

  const EN = {
    appName: "Nintendo Dev Agent", files: "Files", settings: "Settings", faq: "FAQ",
    ok: "OK", cancel: "Cancel", close: "Close", open: "Open", view: "View", save: "Save", copy: "Copy", copied: "Copied.", copyFailed: "Could not copy.",
    refresh: "Refresh", loading: "Loading…", name: "Name", size: "Size", type: "Type", folder: "Folder", file: "File", up: "Up one folder",
    download: "Download", downloading: "Downloading {0}…", downloaded: "{0} downloaded.", upload: "Upload", rename: "Rename", renamed: "Renamed.", restore: "Restore",
    filter: "Filter…", copyPath: "Copy path", itemsCount: "{0} item(s)", selectedN: "{0} selected", emptyFolder: "Empty folder.", noMatches: "No matches.",
    dropHint: "Drag files or folders here to upload.", dropTo: "Drop to upload to {0}",
    newFolder: "New folder", folderName: "Folder name", newFolderDefault: "New folder", creatingFolder: "Creating the folder (the console takes ~6 s)…",
    newFile: "New file", fileName: "File name", newName: "New name",
    moveToTrash: "Move to trash", confirmTrash1: "Move “{0}” to the trash?", confirmTrashN: "Move {0} items to the trash?",
    trashNote: "Nothing is really deleted: it goes to the .ndp-trash folder and can be restored.", movedToTrash: "{0} item(s) in the trash.", moved: "{0} item(s) moved.", restoredTo: "Restored to {0}",
    trashBanner: "You are in the trash. Use “Restore” (right-click) to put an item back where it came from.",
    readOnlyBanner: "The console is in READ-ONLY mode.", readOnlyHow: "To upload or change files, press X on the console (DEVELOPMENT mode). This page cannot change that: on purpose, only whoever holds the console decides.",
    folderReadOnly: "This folder is read-only: the console has not opened it for writing. To change that, press A on the console (Access folders).",
    notWritableHere: "Cannot write to this folder (the console has not opened it for writing, or it is a protected zone).",
    notOpenedTitle: "This folder is not opened", notOpenedBody: "The owner of the console has to open it: press A on the console, pick the folder and use Y to change its level.",
    cannotList: "Could not list this folder",
    uploadingN: "Uploading ({0})", uploadsDone: "Uploads finished", clearFinished: "Clear finished", queued: "Queued", hashing: "Checking the file…", uploaded: "Uploaded", skipped: "Skipped", canceled: "Canceled", failed: "Failed",
    alreadyExists: "A file with that name already exists", existsBody: "“{0}” already exists in this folder. What now?", applyToAll: "Apply to all the next ones", skip: "Skip", keepBoth: "Keep both", replace: "Replace", replaceKeepBackup: "Replace and keep a copy (.bak)",
    uploadFolders: "Upload folders", uploadFoldersNote: "{0} folder(s) will be created on the console (each takes ~6 s). Continue?",
    uploadFinished: "{0} file(s) uploaded.", uploadFinishedErrors: "{0} uploaded, {1} failed.",
    cannotShowImage: "Could not show this image.", binaryFile: "Binary file: showing the first bytes as hex. Use Download to get the file.",
    tooBigToEdit: "Too big to edit here ({0}). Showing only the beginning ({1}). Use Download.", lines: "lines", modified: "modified", readOnlyFile: "read-only",
    readOnlyFileHow: "You can read but not save: the console does not allow writing this file (read-only mode, read-only folder or protected zone).",
    keepBackup: "Keep the previous version (.bak)", saved: "{0} saved.", backupKept: "Previous version kept as {0}.", saveFailed: "Not saved",
    unsavedTitle: "Unsaved changes", unsavedBody: "If you close now, the changes to this file will be lost.", discard: "Discard",
    "name.empty": "Type a name.", "name.chars": "The name cannot contain \\ / : * ? \" < > | or control characters.", "name.trailing": "The name cannot end with a dot or a space.",
    "name.tilde": "Avoid “~” followed by a digit in the name (the SD card confuses it with short names).", "name.long": "Name too long (255 bytes at most).",
    connecting: "Connecting to the console…", cannotReach: "Could not reach the console", cannotReachBody: "Check that the Nintendo Dev Agent app is open on the 3DS, that this computer is on the same Wi-Fi network, and that this address is the one on the console's top screen (the IP changes when the console reconnects).",
    retryNow: "Try now", connectionLost: "Connection lost.", retryingIn: "Retrying in {0} s.",
    "state.connecting": "Connecting", "state.pairing": "Needs pairing", "state.ready": "Connected", "state.offline": "Offline",
    "mode.READ_ONLY": "Read-only", "mode.DEVELOPMENT": "Development",
    pairTitle: "Pair this browser", pair: "Pair", pairStep1: "On the 3DS, press Y: the console shows a code on the TOP screen (valid for 2 minutes, works once).", pairStep2: "Type the code below and give this browser a name.", pairStep3: "Done: the console starts accepting this browser.",
    pairCode: "Code from the console", pairLabel: "Name of this browser", pairLabelHint: "Up to 15 characters. Only to help you remember what was paired.", pairRemember: "Remember on this browser (stores the key here)", pairBadCode: "That code does not look right: it has 16 characters, like XXXX-XXXX-XXXX-XXXX.",
    pairFailedHint: "If the code expired or was already used, press Y on the console again.", pairRejected: "The console did not recognize the key stored in this browser (was it forgotten on the console?). Pair again.",
    pairWhy: "The code never travels over the network: only a proof derived from it. The key stays in this browser and every request after that is signed with it.", pairWindowClosed: "The pairing window is closed on the console: press Y on it to open it.",
    connection: "Connection", address: "Address", status: "Status", agentVersion: "Agent version", platform: "Platform", protocol: "Protocol", mode: "Mode", latency: "Latency", security: "Security", authRequired: "Pairing + HMAC signatures", authOff: "Off (tests only)",
    console: "Console", model: "Model", firmware: "Firmware", ram: "RAM", sdCard: "SD card", systemMemory: "System memory", appMemory: "Application memory", usedOf: "{0} of {1} used", freeN: "{0} free", noDeviceInfo: "The console did not report this data.", notConnected: "Not connected.",
    folders: "Opened folders", foldersNote: "These are the folders the owner opened on the console. They can only be changed on the console itself (A button): this page just shows them.", levelWrite: "Read and write", levelRead: "Read only",
    noWriteFolders: "No writable folders.", noReadFolders: "No other folders.", protectedNote: "Protected zones (/Nintendo 3DS, /luma, /boot.firm, /gm9, /private) never accept writes, even when opened.",
    pairing: "Pairing", pairedThis: "This browser is paired as “{0}”.", notPairedThis: "This browser stored no key (this tab's session only).", forgetNote: "Forgetting removes the key from here; the console keeps it until you press SELECT twice on it.", forgetThis: "Forget this browser", forgetThisBody: "The key will be removed from this browser and you will have to pair again.",
    preferences: "Preferences", language: "Language", langAuto: "Automatic", theme: "Theme", themeAuto: "Automatic", themeLight: "Light", themeDark: "Dark",
    showHidden: "Show hidden items", showHiddenHint: "Names that start with a dot.", confirmDelete: "Ask before moving to the trash", confirmDeleteHint: "Nothing is really deleted, but it asks anyway.", backupOnSave: "Keep a copy (.bak) when saving by default", backupOnSaveHint: "Checked by default in the editor.",
    "err.UNSUPPORTED_PROTOCOL": "Incompatible protocol version.", "err.UNAUTHORIZED": "Not authorized: pair this browser.", "err.FORBIDDEN_MODE": "The console is read-only: press X on it.", "err.PROTECTED_PATH": "Folder not opened or protected: press A on the console to open it.",
    "err.NOT_FOUND": "Not found.", "err.EXISTS": "Already exists.", "err.IO_ERROR": "SD card read/write error.", "err.NO_SPACE": "No space left on the SD card.", "err.BAD_REQUEST": "Invalid request.", "err.BUSY": "The console is busy: try again in a moment.",
    "err.TOO_LARGE": "Too large.", "err.HASH_MISMATCH": "The content does not match (SHA-256): nothing was written.", "err.TIMEOUT": "The console took too long to answer.", "err.UNSUPPORTED_COMMAND": "The console app is too old for this: update it.", "err.HELLO_REQUIRED": "Connection out of order.", "err.BAD_FRAME": "Invalid message.", "err.PATH_INVALID": "Invalid path or name.",
    "faq.intro": "Quick answers. Click a question to open it.",
    "faq.what.q": "What is this page?",
    "faq.what.a": "It is the file manager of your 3DS, served by the Nintendo Dev Agent app running on the console itself. From here you browse, view, edit, upload (by drag and drop), download, rename and delete files on the SD card, like an FTP, with nothing to install.\nIt only works while the app is open on the console and this computer is on the same Wi-Fi network. The MCP server for AI assistants keeps working at the same time, unchanged.",
    "faq.pairing.q": "How do I connect and pair this browser?",
    "faq.pairing.a": "The console only serves computers/browsers you authorized. The first time: press Y on the 3DS (the code shows on the top screen for 2 minutes) and type it on the pairing screen. The key is stored in this browser (or only until the tab closes, if you uncheck “Remember”).\nTo remove it: Settings → Forget this browser. To forget every computer at once: press SELECT twice on the console.\nOnly one browser tab is connected at a time: opening the page in another tab disconnects the previous one.",
    "faq.keys.q": "What does each console button do?",
    "faq.keys.a": "- A: opens “Access folders”, where you choose which folders this page and the assistant may see and change (Y cycles a folder's level: none / read / write).\n- X: toggles READ-ONLY ⇄ DEVELOPMENT. The app always starts in read-only.\n- Y: opens pairing (shows the code).\n- SELECT twice: forgets every paired computer.\n- START: quits the app.",
    "faq.modes.q": "Why can't I change files?",
    "faq.modes.a": "Three locks, all on the console:\n- Mode: in READ-ONLY you can only read and download. Press X on the console for DEVELOPMENT mode.\n- Folders: you can only write in folders opened for writing (default: /3ds/nintendo-dev-agent). Change it with the A button on the console; see the list in Settings.\n- Protected zones: /Nintendo 3DS, /luma (except plugins and titles), /boot.firm, /gm9 and /private never accept writes.\nThis page cannot change any of this, for safety: whoever holds the console decides.",
    "faq.upload.q": "How do I upload files? What are the limits?",
    "faq.upload.a": "Drag files or folders anywhere on the Files tab (or onto a folder in the list), or use the Upload button.\nEach file is checked here, sent, and the console verifies size and SHA-256 before putting it in place: an interrupted upload never leaves a half-written file. If the name already exists the page asks: skip, keep both, replace, or replace and keep a .bak copy.\n- Speed is that of the 3DS Wi-Fi: about 0.6 to 1 MiB/s. A 100 MiB file takes a few minutes.\n- One upload at a time, and the console serves one thing at a time: keep the page open.\n- Creating a folder takes ~6 s (that is how the console is).",
    "faq.edit.q": "How do I edit a file?",
    "faq.edit.a": "Double-click a text file (up to 1 MiB) to open the editor; Ctrl/Cmd+S saves. Tick “Keep the previous version” to keep a .bak copy. Images open in a preview, binary files show their first bytes as hex, and big files can only be downloaded.\nSaving is atomic like an upload: if anything fails, the original file stays intact. The editor uses UTF-8.",
    "faq.trash.q": "What happens when I delete? Can I get it back?",
    "faq.trash.a": "Nothing is really deleted: the item goes to the .ndp-trash folder inside the opened folder. Open .ndp-trash, right-click and choose Restore. The page does not empty the trash; to really free space, delete from a computer with the SD card.",
    "faq.mcp.q": "How do I install the local MCP (for Claude Code / Codex)?",
    "faq.mcp.a": "The MCP lets AI assistants use the console through your computer. It is independent from this page (the page talks to the console straight from the browser; the MCP runs on your computer) and both work together.",
    "faq.mcp.s1": "Install Node.js 22.18 or newer and check:", "faq.mcp.s2": "Download the project and install its dependencies:", "faq.mcp.s3": "Tell it where the project folder is:", "faq.mcp.where": "Project folder on this computer", "faq.mcp.s3b": "The commands below adjust as you type.",
    "faq.mcp.s4": "Pair the computer with the console (once). Press Y on the console and run this, typing the code from its screen:", "faq.mcp.s4b": "This is a separate pairing from this browser's: each computer pairs on its own. {0} is the console's IP right now.",
    "faq.mcp.s5": "Register the server in your assistant:", "faq.mcp.s5b": "--local-root is the folder on THIS computer the assistant may upload from and download to. Change it to whatever you prefer.",
    "faq.mcp.s6": "Restart the assistant session and ask, for example: “list the files on my 3DS”.", "faq.mcp.s6b": "Write tools only work while the console is in DEVELOPMENT mode (X). The console's IP changes, but the MCP finds it on its own on the network.",
    "faq.mcp.remove": "To remove it:", "faq.mcp.more": "Full details in",
    "faq.security.q": "Is it safe?",
    "faq.security.a": "The page is served over plain HTTP on your local network. After pairing, every message is signed with HMAC (nobody can forge requests), but file contents are NOT encrypted: anyone sniffing your Wi-Fi can see them. Use it only on networks you trust.\nThe console rejects requests whose address or origin do not match (protects against a malicious website trying to reach the console through your browser), accepts no form uploads, and drops idle connections. Nothing leaves the console for the internet.\nThe key lives in this browser's storage: anyone using this browser profile can use it. On a shared computer, untick “Remember” or use “Forget this browser”.",
    "faq.trouble.q": "Something went wrong. Now what?",
    "faq.trouble.a": "- It won't open: the app must be open on the 3DS, this computer on the same Wi-Fi, and the address must match the console's top screen (the IP changes when it reconnects).\n- “Read-only”: press X on the console.\n- Folder not opened (PROTECTED_PATH): press A on the console and open the folder.\n- Busy (BUSY): the console does one thing at a time; wait a few seconds.\n- Slow or dropping: the 3DS Wi-Fi sleeps when idle and the first action after a pause is slow; the page reconnects by itself.\n- Pairing window closed: press Y again.\n- Console froze with a “mcu” error after opening the Rosalina menu: in our tests this happened once with the app open; close the app before using Rosalina and reboot the console.",
    "faq.about.q": "About",
    "faq.about.a": "Nintendo Dev Agent is free software (Apache-2.0). Source, releases and issues: https://github.com/BC-Estrategias/nintendo-dev-mcp\nThis page and the console app are one program; the version is shown in Settings.",
  };

  const DICT = { "pt-BR": PT, en: EN };
  const LANG_KEY = "ndev.lang";
  let choice = "auto";
  try { const v = localStorage.getItem(LANG_KEY); if (v === "pt-BR" || v === "en") choice = v; } catch (_) { /* automatic */ }

  const detect = () => (/^pt/i.test((globalThis.navigator && navigator.language) || "") ? "pt-BR" : "en");
  const lang = () => (choice === "auto" ? detect() : choice);
  function setLang(c) {
    choice = c === "pt-BR" || c === "en" ? c : "auto";
    try { if (choice === "auto") localStorage.removeItem(LANG_KEY); else localStorage.setItem(LANG_KEY, choice); } catch (_) { /* not persisted */ }
  }
  const has = (key) => Object.prototype.hasOwnProperty.call(DICT[lang()], key);
  function t(key, ...args) {
    const d = DICT[lang()];
    const s = Object.prototype.hasOwnProperty.call(d, key) ? d[key] : key;
    return args.length ? s.replace(/\{(\d+)\}/g, (m, i) => (args[+i] !== undefined ? String(args[+i]) : m)) : s;
  }

  NDP.i18n = { t, has, lang, setLang, choice: () => choice, DICT };
})();
