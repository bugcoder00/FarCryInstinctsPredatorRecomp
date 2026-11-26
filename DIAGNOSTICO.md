# Ferramenta de diagnóstico

Ela registra tudo o que falta ou falha enquanto o jogo roda. Não precisa ligar nada: ela está sempre ativa.

## O relatório: `FCRecomp\build\diagnostico.txt`

É escrito quando:
- o jogo é fechado (janela, Ctrl+C ou `tools\run.ps1`/`screenshot.ps1`);
- o jogo trava;
- você aperta **F8** com a janela do jogo em foco.

O relatório separa os problemas por tipo, do mais frequente para o menos frequente. Cada linha mostra quantas vezes o problema aconteceu e em que quadro apareceu pela primeira vez:

| Seção | O que significa |
|---|---|
| Travamentos | Exceções, com a pilha do jogo, e chamadas para funções que não existem |
| Funções do sistema que ainda não existem | Funções do kernel/XAM que o jogo chamou, com os argumentos e o endereço de quem chamou |
| Partes de funções do sistema | Classes de arquivo, tipos de objeto etc. ainda sem suporte |
| Arquivos não encontrados | Caminho no PC e motivo |
| Comandos da GPU ignorados | Pacotes PM4 que o emulador ainda não trata |
| Recursos da GPU sem suporte | Tipos de primitiva e formatos de textura (o desenho ou a textura somem) |
| Shaders com problema | Shaders que não compilaram (o desenho é pulado) ou que usam instruções ainda não traduzidas |

## Teclas (com a janela do jogo em foco)

| Tecla | Função |
|---|---|
| **F5** | Alterna os shaders entre "linha reta" (rápido, o padrão) e "laço" (o modo antigo). Se algo muda ao apertar, por exemplo um texto que aparece ou some, o problema está em algum shader traduzido em linha reta. |
| **F6** | Liga/desliga os mipmaps das texturas |
| **F7** | Liga/desliga o limite de cor 0–1 (alvos de 8 bits do Xbox) |
| **F8** | Escreve o relatório agora |
| **F12** | Pacote do bug em `build\bugs\bug_N\` (save do momento, captura do quadro como o F9, log, relatório, info.txt). Para voltar ao ponto: `tools\carregar_bug.ps1 -Bug N [-Segundos 60] [-Prints "5,20"] [-Extra "FCRECOMP_PERF=1"]` |
| **F9** | Captura o próximo quadro em `FCRecomp\build\diag\quadro_N\`: `desenhos.txt` (todos os desenhos, com shaders e superfícies) e uma imagem `.bmp` de cada etapa intermediária (cada "resolve"). A descrição completa de cada desenho também vai para `fcrecomp_log.txt` (linhas `[draw] DRAW_INDX`). |

O título da janela mostra os interruptores ligados, por exemplo `[F5 laco]`, e `[F9 capturando]` enquanto a captura estiver em andamento.

## Como usar para caçar um erro

1. Rode o jogo até o ponto do erro, por exemplo o menu com textos faltando.
2. Aperte **F9** para capturar o quadro com o erro.
3. Aperte **F5**, **F6** ou **F7** e veja se o erro some. Se sumir, aperte **F9** de novo para capturar o quadro certo e comparar as duas capturas.
4. Feche o jogo e leia `diagnostico.txt`.

## Ferramentas de investigação (variáveis de ambiente)

| Variável | O que faz |
|---|---|
| `FCRECOMP_MEMDUMP=quadro` (ou **F11**) | Copia a memória do jogo para `diag\memoria_N\` (um arquivo por região, com o endereço do Xbox no nome) |
| `FCRECOMP_WATCH=end:tam,end:tam@quadro` | Ponto de observação: registra cada escrita nessas faixas (função recompilada, quem chamou, registradores, valor antes/depois). `@quadro` começa a observar só a partir desse quadro; `FCRECOMP_WATCHMAX` é o limite de anotações (padrão 400) |
| `FCRECOMP_STRWATCH=texto` | Registra toda cópia de memória do jogo (memcpy_s) que contenha o texto, em ASCII ou Unicode, com a pilha de chamadas |
| `FCRECOMP_MISSAO=10,40:100` | Captura (como o F9) os quadros 10 e 40 depois do início da missão e fecha o jogo no quadro 100 depois dele. Use para testes curtos e repetíveis |
| `FCRECOMP_DRAWSHOT=c:i,j` | No quadro capturado, salva a imagem depois dos desenhos i, j... (acha qual desenho estraga a cena) |
| `FCRECOMP_DRAWSHOT=c:pt=HASH` / `c:tr` | Igual, depois de cada desenho desse pixel shader / de cada desenho transparente (com mistura: partículas, brilhos, fumaça). Os números dos desenhos mudam de uma rodada para outra; assim não precisa saber antes. Em `desenhos.txt`, `zc=` é o RB_DEPTHCONTROL (bit 1 = teste de profundidade) e `semprof` = sem profundidade |
| `FCRECOMP_RESOLVELOG=0286E000` ou `*960-1100` | Anota cada cópia da GPU (resolve) para esses endereços, ou todas nesses quadros |
| `FCRECOMP_NOPERSIST=1` / `FCRECOMP_NOPIXSHIFT=1` | Desligam as correções da tela de carregamento e do meio pixel (para comparar) |
| `FCRECOMP_FAIXAS=1` | Volta a desenhar a tela em duas faixas como o Xbox (o padrão desenha a tela inteira de uma vez, metade do trabalho de geometria) |
| `FCRECOMP_SPINWAIT=1` | Volta às esperas em giro (thread de renderização do jogo esperando a GPU, agendador de tarefas do jogo consultando a hora e processador de comandos); o padrão agora dorme entre as verificações |
| `FCRECOMP_TEMPOLOG=1` | Mostra no log quem chama a função de hora do jogo com mais frequência |
| `FCRECOMP_ROTEIRODIAGONAL=1` | Na repeticao de um roteiro, as diagonais do teclado gravadas (W+A, W+D...: 32767,32767 antes de 26/11, ou 23170,23170 da correcao anterior) passam pela correcao atual (26400 em cada eixo, a mesma velocidade de andar reto); `=n` usa n em cada eixo; `@tique` no fim (`26400@680`) so a partir desse tique (para o caminho nao mudar antes de pegar o feral, por exemplo) |
| `FCRECOMP_POSLOG=1` | Mostra no registro, a cada tique do roteiro, a posicao do jogador (`[pos] t=tique ms=hora x y z`; objeto em jogador+72, +4/+8/+12, achado no editor de mapas) para medir a velocidade de andar (distancia por segundo) |
| `FCRECOMP_NOWARM=1` | Desliga o aquecimento (`build\warmcache`: pipelines e superfícies recriados ao abrir o jogo) |
| `FCRECOMP_TRAVADA=50` | Junto com `FCRECOMP_PERF=1`: anota no log cada quadro que passou de 50 ms, com o tempo real da GPU e o que o quadro gastou (espera da GPU, texturas e pipelines novos) |
| `FCRECOMP_TECLADO=texto` | Responde sozinho a caixa de digitação do MENU DE TRAPAÇAS com esse texto (para testes automáticos) |
| `FCRECOMP_SEMASSINC=1` | Volta a compilar shaders e pipelines novos na hora (o jogo para até terminar). O padrão compila em segundo plano: o quadro espera no máximo 8 ms e o objeto aparece alguns quadros depois |
| `FCRECOMP_COMPILADORES=n` | Quantas threads compilam shaders/pipelines em segundo plano (padrão: núcleos − 2, entre 2 e 6; antes do bug 29 eram 2). O registro mostra `[draw] fila de compilacao vazia: N ... em X s` |
| `FCRECOMP_SEMURGENTE=1` | Desliga a exceção das "passadas de tela" (cópia da cena para a tela, pós-processo, HUD), que são compiladas na hora mesmo com a compilação em segundo plano (bug 28: tela preta com HUD borrado no início da fase com o cache vazio) |
| `FCRECOMP_SEMSEGURAR=1` | Desliga a espera do item 33: com 24 ou mais shaders/pipelines na fila no fim de um quadro (entrada numa fase com o cache vazio), o quadro incompleto não é mostrado e a placa espera a fila esvaziar com o relógio do jogo parado. O registro mostra `[draw] imagem parada enquanto compila: N ... esperou X s` |
| `FCRECOMP_SEMURGENTE34=1` | Desliga o item 34: bloco do terreno (8446 índices) sem shader pronto volta a ficar de fora com o quadro mostrado (chão azulado na 21_DUNEA sem cache) e caixa de oclusão pulada volta a contar 0. O registro mostra `[draw] quadro com desenho grande sem shader nao mostrado: N compilacoes, esperou X ms` |
| `FCRECOMP_PULADOSLOG=1` | Anota cada desenho que ficou de fora por shader/pipeline ainda compilando: `[pulado] qQUADRO shader/pipeline vs=… ps=… n=… prim=… prof=… blend=… mask=…` (muitas linhas) |
| `FCRECOMP_SEMPREPARO=1` | Desliga o item 35: não prepara os pixel shaders do `shaders.dat` na primeira abertura. Para refazer a preparação apague `shadercache\jogo_preparado.txt`. O registro mostra `[preparo] 582 pixel shaders ... prontos no shadercache em X s` |
| `FCRECOMP_PRESENTLOG=inicio-fim` | Mostra como cada quadro entre esses números foi montado e entregue (pedaços resolvidos). Útil para quadros pretos: desde o item 37 um quadro segurado (itens 33/34) mantém a imagem anterior em vez de cair na memória de tela do Xbox (preta) |
| `FCRECOMP_PULARCENA_SEMJOGADOR=1` | "PULAR CENA" no menu de pausa volta a aparecer com qualquer BeginCutScene do script; o padrão exige também o jogador preso na cena (câmera presa, `BindCarverEyes`, ou animação dele, `PlayAnimCarver`), para um BeginCutScene falso de checkpoint (02_native, Crowe_Hostage) não oferecer o pulo. `[pularcena] menu de pausa aberto` no log mostra o porquê |
| `FCRECOMP_PULARCENA_FALSA=0` | Não encerra o BeginCutScene falso. Por padrão, um BeginCutScene em que o jogador não é preso (câmera ou animação) em 4 s de jogo é encerrado pelo próprio `Mission:Event__GUI_EndCutScene` (volta o HUD e o controle; checkpoint da 02_native depois da cena da Valerie refém). Log: `[pularcena] BeginCutScene sem o jogador preso` |
| `FCRECOMP_PULARCENA_LEGENDA=0` | Não tira a legenda da fala no PULAR CENA. Por padrão, ao parar as falas da cena (e no fim do pulo) a legenda é tirada como o gerente de som faz (`+40 = -1`, `+44 = 1` na legenda do jogo): antes o som parava e a legenda continuava passando por alguns segundos |
| `FCRECOMP_PULARCENA_SONSANIM=0` | Não cala, depois do PULAR CENA, os sons que vêm da animação da cena. Por padrão, os grupos de som postados pelo caminho das animações (retorno 82617A78) durante a cena, e que não apareceram fora dela, ficam calados depois do pulo até a próxima cena (no máximo 3 min). Antes a animação da cena às vezes continuava e soltava as falas do Carver no meio do jogo (roteiro 2026-11-26_09-30) |
| `FCRECOMP_PULARCENA_SONSATOR=0` | Não cala, depois do PULAR CENA, os sons de animação de um **ator da cena** (personagem das ações PlayAnimation/PlaySound da cena achado na pilha de sub_824B8020) cujo grupo ainda não apareceu fora de cena. Cobre o caso em que o pulo vem antes da primeira chave de som da animação (roteiro 2026-11-26_13-03, 26_shantynight: pulo ~3 s depois do começo da cena): nenhum grupo tinha sido visto na cena e a animação do Carver continuava soltando as falas 03A2 itens 1, 3, 4 e 6 (+10 a +30 s). Teste: `FCRECOMP_PULARCENA_AUTO=3.0` com `toolsepetir_roteiro.ps1 -Ate 300` |
| `FCRECOMP_FALALOG=1` | Registra cada fala tocada na voz de um personagem (sub_8223FDA0), com o som devolvido e a cadeia de retornos (`[fala]`) |
| `FCRECOMP_MIRA=porcentagem` | Tamanho da mira nos testes (sem launcher; `100` = o do jogo, de 25 a 150). A opção do launcher e da janela F2 é "Tamanho da mira" (`mira` no opcoes.ini). mira.cpp escala os retângulos do elemento `i_crosshair` do HUD depois de sub_821FA380 |
| `FCRECOMP_MIRALOG=1` | Anota a cada 2 s os retângulos da mira (`[mira]`: as três chaves da animação do elemento, quantas vezes o HUD a montou) |
| `FCRECOMP_SAVESTATE=1` | Liga as teclas do save state (`save_state.cpp`; padrão F5 salva, F9 carrega o mais novo) nos testes: com `FCRECOMP_*` (modo teste) elas ficam desligadas para não atrapalhar as teclas F5–F11 do diagnóstico. O save state usa o checkpoint do próprio jogo (`Game:SaveCheckpoint`/`LoadCheckpoint` pelo Lua) e guarda uma cópia do checkpoint de cada estado (até 10 por fase) em `<pasta do perfil>\savestates\<fase>\*.fcss` (a fase é o `g_LevelName` do jogo; ao entrar numa fase a lista dela é lida do disco: `[savestate] N estados da fase X lidos do disco`). Nos testes a pasta do perfil é a `save_teste`, que o `repetir_roteiro.ps1` apaga a cada abertura. O save do jogo não muda. No log: `[savestate] ...` (inclusive `memoria do checkpoint gravada pelo jogo N quadros depois`) |
| `FCRECOMP_SAVESTATE_REPOR=N` | Quadro, depois do carregamento, em que o save state põe o jogador na posição e no ângulo guardados (z + 0,3, para não cair do chão), padrão 3. O log anota a posição nesse quadro e 3 quadros depois (`[savestate] quadro N depois do carregar: jogador em: ...`). Num veículo a posição não é mexida. As respostas dos trechos Lua do save state são procuradas na memória do jogo numa thread à parte (`resposta achada em X ms, fora do jogo`): na thread do jogo a busca travava 70 a 400 ms a cada trecho |
| `SAVESTATE=salvarN` / `carregarN` / `carregarN:pos` no `ao_vivo.txt` | Salva um estado / carrega o mais novo / carrega o da posição `pos` da lista (1 = o mais novo). Mude o `N` a cada pedido (o mesmo pedido não é repetido). Precisa de `FCRECOMP_SAVESTATE=1` no modo teste |
| `FCRECOMP_SONSCENA_ENDERECO=1` | PULAR CENA: volta a reconhecer "sons de antes da cena" só pelo endereço. Por padrão, um som que sai da lista do gerente de vozes deixa de contar como "de antes" (o endereço é reaproveitado: a voz do Carver podia nascer nele e continuar tocando depois do pulo, às vezes sim, às vezes não) |
| `FCRECOMP_GRAMAPROF=0` | Desliga a passada extra de profundidade da grama misturada (ps `62EBA53F580D516D`): por padrão, depois de cada desenho dela o mesmo desenho é repetido só na profundidade com alpha test 0,5, para a cachoeira e outros transparentes desenhados depois não aparecerem por cima da grama (bug 90, 02_native) |
| `FCRECOMP_DESENHOPERF=1` | A cada 120 quadros, escreve no log uma linha `[etapas]` com o tempo de CPU de cada etapa da tradução dos desenhos (shader, pipeline, superfícies, texturas, samplers, constantes, vértices, estado, envio) e quantos desenhos reusaram a tabela de texturas. Com `FCRECOMP_PERF=1`, o `[perf]` também mostra o tempo do buffer de comandos (pacotes e registradores por quadro) e quanto o jogo esperou pelas consultas de oclusão |
| `FCRECOMP_SEMREUSOTEX=1` | Monta a tabela de texturas e samplers em todo desenho, como antes. Por padrão, um desenho com as mesmas constantes de leitura de textura que o anterior (mesmo quadro, sem resolve no meio) reusa a tabela dele |
| `FCRECOMP_TABSOANTERIOR=1` | Reusa só a tabela de texturas do desenho anterior, como antes. Por padrão, qualquer tabela já montada no quadro (desde o último resolve) serve para outro desenho com as mesmas texturas. Selva do roteiro 2026-11-25_20-39: montagens 1874 → 374 por quadro, linha de desenho 18,5 → 16,5 ms |
| `FCRECOMP_SHADERSEMATALHO=1` | Toda carga de shader (IM_LOAD) copia o microcode e calcula o hash de novo, como antes. Por padrão, o mesmo tipo, endereço e tamanho, com os bytes iguais (memcmp), devolve o shader já conhecido (LoadShader era ~6,5% da linha de desenho) |
| `FCRECOMP_REGSUMAUM=1` | Escreve os registradores do buffer de comandos um a um, como antes. Por padrão, os blocos seguidos (constantes dos shaders etc.) são copiados de uma vez |
| `FCRECOMP_ENVIOPARCIAL=N` | Envia os comandos já gravados à placa a cada N desenhos do quadro (padrão 400; 0 = só na troca de imagem, como antes), para a placa começar enquanto a CPU traduz o resto. Selva da 02_native: 30 → 33 FPS |
| `FCRECOMP_FOTOCOPIA=1` | Tira a foto da profundidade (caixas da 2ª faixa do tiling) com a cópia inteira da superfície, como antes, em vez do shader que copia só a área da tela |
| `FCRECOMP_FOTOINTEIRA=1` | A foto da profundidade copia as 2048 linhas da superfície em vez de só as da tela (736 na 1x). Selva da 02_native: 2,3 → 0,75 ms de placa por quadro |
| `FCRECOMP_FOTONACONSULTA=1` | Volta ao antigo: a foto da profundidade é tirada com a consulta de oclusão da caixa já aberta, e os pixels da foto entram na contagem. O brilho das lâmpadas das casas aparece por cima dos telhados (roteiro 2026-11-26_09-30, tique 1100) |
| `FCRECOMP_SEMRECEITA=1` | Desliga o item 38: não aplica o `receita_shaders.txt` (ao lado do exe) no warmcache. O registro mostra `[receita] N shaders montados do shaders.dat (...), N pipelines e N superficies acrescentados`. Para aplicar de novo apague `warmcache\receita_aplicada.txt` |
| `FCRECOMP_COLETARRECEITA=1` | (o `coletar_receita.bat` liga) Grava `receitas\receita_<data>.txt` com o warmcache e cada shader/pipeline/superfície novo, no formato da receita. Registro: `[receita] coletando em ...`. Junte com `python tools\gerar_receita.py juntar <pasta>` |
| `FCRECOMP_SEMDRED=1` | Desliga o DRED (item 42; ligado por padrão): sem ele, se a placa de vídeo cair, o registro não diz até qual comando/desenho ela chegou |
| `FCRECOMP_SEMMASCARA=1` | Na troca de programa (jogo ↔ editor, troca de fase) a placa não segura a tela de carregamento enquanto o jogo novo começa escuro (item 30); a janela continua nascendo com a tela de carregamento |
| `FCRECOMP_SOMBRANAN=0` | Desliga a correção da luz do sol sem sombra válida (casas e caixas pretas na vila da fase 21_DUNEA). `=valor` muda a profundidade usada (teste) |
| `FCRECOMP_NANLOG=1` | Anota no log as constantes de shader de vértices com valor inválido (NaN) que o jogo manda para a placa e as luzes corrigidas pelo `FCRECOMP_SOMBRANAN` |
| `FCRECOMP_ROTEIROCAPTURA=etapa:tique` | Durante a repetição de um roteiro, faz a captura de diagnóstico (como F9) nesse tique |
| `FCRECOMP_GPUPROF=1` | Perfilador da GPU: mostra no log quais shaders gastam mais tempo da placa de vídeo. Junto com uma captura, grava `tempos_gpu.txt` (tempo de cada desenho). Resolves, limpezas e a foto da profundidade aparecem entre parênteses, ex. `(resolve cor 1280x720 para 006DA000)`. Para comparar desempenho, use `FCRECOMP_ENVIOPARCIAL=0` junto (e no `-Extra` do `repetir_roteiro.ps1` separe as variáveis com `;`) |
| `FCRECOMP_QUADROLOG=1` | Grava `build\quadros.csv`: cada quadro com a hora real (ms), quantos quadros de som o jogo já entregou e o total de desenhos. Mostra se a imagem anda no ritmo do som |
| `FCRECOMP_XMALOG=1` / `=2` | Log do chip de som XMA (duração de cada som, quadros por segundo). Com `=2`, também os primeiros bytes de cada bloco de som enviado ao chip; `python tools\xma_achar.py` acha esses blocos no `dvdsound.dat` e `python tools\xma_duracao.py` mostra a duração real no disco |
| `FCRECOMP_MOUSELOG=1` | Mostra no log cada leitura do mouse e o valor que ele virou no analógico direito |
| `FCRECOMP_MOUSEPULSO=0.5-6.5:500:1` | Teste de atraso do mouse: de 0,5 a 6,5 s depois da primeira mira na fase, manda 500 movimentos/s de 1 contagem pelo SendInput (passam pelo Raw Input como um mouse de verdade). A cada segundo o log mostra `[mousepulso]`: movimentos, contagens somadas ao giro (1:1), tempo até chegar no runtime (entrega) e até a mira somar à câmera. Numa repetição de roteiro o giro gravado sai e entra o mouse: `tools\repetir_roteiro.ps1 -Gravacao 2026-11-25_14-10 -Extra "FCRECOMP_MOUSEPULSO=0.5-6.5:500:1"` |
| `FCRECOMP_MOUSEJANELA=1` | Raw Input do mouse na janela do jogo (como antes), em vez da thread própria "mouse cru"; para comparar |
| `FCRECOMP_CAMTARDIA=0` | Desliga o giro sem atraso (`camera_tardia.cpp`): câmera, braços, arma e corpo voltam a mostrar o giro do mouse/analógico um quadro depois, como no jogo original. Ligado, o giro já chegado é somado aos ângulos do jogador e à cópia deles no corpo do personagem (entidade +932 montado, +884 a pé) antes da atualização dos personagens e desfeito antes do jogo aplicar o giro. Medido com fotos por quadro: roteiro `2026-11-25_14-10` (a pé, jogo) cena 29 de 29 e arma 27 de 29 quadros iguais ao quadro seguinte sem ele; roteiro `2026-11-25_14-47` (editor, quadriciclo) cena 51 de 52 e arma 52 de 52; a pistola não escorrega contra o giro |
| `FCRECOMP_CAMTARDIALOG=1` / `=2` | `1`: anota os giros adiantados, a conferência (giro que o jogo aplicou × previsto), quando o corpo não tem a cópia dos ângulos e quando o personagem passa a pé/montado (com o tique do roteiro). `2`: ordem do passo na thread do jogo (câmeras montadas e de onde copiam os ângulos, fim da mira) com a hora de cada etapa (ms), por 0,3 s a partir de `FCRECOMP_CAMSEQT` segundos (padrão 4) depois da primeira mira; com `FCRECOMP_CAMSEQVIGIA=1` vigia quem lê os ângulos do jogador e com `FCRECOMP_CAMSEQFONTE=<lr em hexa>` quem escreve os ângulos que a câmera chamada desse endereço copia (vigia de memória, `FCRECOMP_WATCHMAX` limita) |
| `FCRECOMP_QUADRONAHORA=1` / `=0` | Quadro na hora (`quadro_na_hora.cpp`): durante o jogo a troca de imagem sai na hora em vez de esperar o vblank emulado, e o ritmo de 30/60 FPS é feito dormindo no começo do passo, antes de ler o mouse. Editor: ligado por padrão — leitura do mouse → Present de 33 para 11-12 ms (roteiro `2026-11-25_14-47`: a pé, tiques 114-184, 32 → 12,6 ms; no quadriciclo 12,4 ms), trocas regulares. Jogo: desligado por padrão — de 81 para 46 ms, mas as trocas ficam irregulares (picos de 70-87 ms contra 35 no original, roteiro `2026-11-25_14-10`): a thread de desenho do motor só entrega o quadro depois do passo seguinte e a espera do vblank escondia essa variação. `=1` liga para testar, `=0` desliga no editor |
| `FCRECOMP_QUADROLOG=1` | A cada ~2 s: quanto o passo dormiu, intervalo médio e maior entre trocas durante o jogo e passos que chegaram atrasados (funciona com o quadro na hora ligado ou desligado) |
| `FCRECOMP_DESENHONOVO=0` / `FCRECOMP_DESENHOLIMITE=ms` | Desenho do passo atual (só o jogo, ligado por padrão, `quadro_na_hora.cpp`): a thread de desenho espera, no começo do quadro dela, o passo em andamento montar a câmera, no máximo 12 ms (`DESENHOLIMITE`). Leitura do mouse → Present ~53 → 31-38 ms (roteiro `2026-11-25_14-10`), trocas a 33,3 ms como sem ela. Com `FCRECOMP_QUADROLOG=1` anota a espera média/maior e quantas passaram do limite. Limite 30 perdia um vblank a cada ~2 s. `=0` desliga |
| `FCRECOMP_SAVELOG=1` | Mostra no log cada chamada de usuário, perfil e saves (XamUser*, XamContent*, notificações). Desde o item 32 os saves ficam na pasta do perfil do launcher (`opcoes.ini perfil=`; vazio = `save\`, outro = `save\perfis\<nome>\`); o log mostra "jogos salvos: ... (perfil ...)" |
| `FCRECOMP_SAVEDIR=pasta` | Usa outra pasta de saves (o `carregar_bug.ps1` usa `build\save_teste`) |
| `FCRECOMP_LANLOG=1` / `=2` | Rede (SYSTEM LINK): anota sockets, envios e recebimentos (até 200), mensagens de sessão e `XNetXnAddrToInAddr`; com `=2` também o conteúdo dos primeiros 150 pacotes em hexadecimal |
| `PREDATOR_LAN_IP=127.0.0.2` | Teste de rede com várias cópias no mesmo PC (as outras com .3, .4): endereço próprio, broadcast para 127.0.0.1–8, registro `fcrecomp_log_lan_2.txt` |
| `PREDATOR_ONLINE_HOST=ip[:porta]` | SYSTEM LINK pela internet: procura a partida direto nesse IP (ou nome) do anfitrião, além do broadcast. É o mesmo que o campo "IP do anfitrião" do launcher (a variável vale mais). `porta` = porta de busca do anfitrião (padrão 1004) |
| `PREDATOR_ONLINE_IP_PUBLICO=ip` | IP de internet deste PC (anunciado no endereço do Xbox), em vez de perguntar ao roteador |
| `PREDATOR_UPNP=0` / `=1` | Desliga o UPnP (consulta do IP de internet e abertura automática das portas no roteador) / liga no teste no mesmo PC, onde vem desligado |
| `PREDATOR_SEMBROADCAST=1` | Não manda broadcast: a busca e o anúncio da partida só vão pelo caminho direto do IP (teste do modo internet) |
| `PREDATOR_NAT_TESTE=1` | Com `PREDATOR_LAN_IP`: finge que a cópia está atrás de um roteador (portas do jogo +300; IP local anunciado 192.168.77.x, que ninguém alcança) |
| `FCRECOMP_VOZLOG=1` | Chat de voz (headset do Xbox no microfone/alto-falantes do PC, `voz.cpp`): anota `XamVoiceCreate`, os primeiros pacotes do microfone e do fone e depois 1 a cada 250 (tamanho, formato, pico) e os pacotes recebidos com voz |
| `FCRECOMP_VOZ=0` | Sem chat de voz: o jogo acha que não há headset (como antes de 26/11) |
| `FCRECOMP_VOZ_TESTE=hz` | No lugar do microfone, um bipe de `hz` (0,4 s sim, 0,4 s não): teste da voz com duas cópias (`PREDATOR_LAN_IP`), cada uma com uma frequência |
| `FCRECOMP_VOZ_WAV=arquivo.wav` | Grava a voz recebida (o que toca no "fone", 8 kHz) nesse arquivo, regravado a cada 2 s |
| `PredatorGame.exe --upnp-teste` | Confere o UPnP do roteador: pergunta o IP de internet, abre as portas do jogo e fecha de novo; resultado em `upnp_teste.txt` |
| `FCRECOMP_AUTOINPUT="...,M6:A,F4-20:LUP"` | Itens com `M` contam os segundos a partir do menu principal, com `F` a partir do início da fase (precisa de `FCRECOMP_MISSAO`). `inicio-fim:BOTAO` segura o botão. Analógicos: `LUP`/`LDOWN`/`LLEFT`/`LRIGHT` (andar), `RUP`/`RDOWN`/`RLEFT`/`RRIGHT` (olhar); gatilhos `LT`/`RT`. No `carregar_bug.ps1`: `-Roteiro "F4-20:LUP"` |
| `FCRECOMP_OCLUSAOLOG=1` / `FCRECOMP_OCLUSAOFIXA=1` | Mostra no log quantos pixels cada consulta de oclusão contou / volta ao antigo "sempre visível" (para comparar halos de luz e objetos que somem) |
| `FCRECOMP_OCLUSAOLOG=t4370-4445` | Igual, mas só nesses tiques de um roteiro gravado, com mais detalhes: a resposta de cada registro (e de qual faixa da tela), as consultas fechadas sem desenho e o número que o jogo vê no fim |
| `FCRECOMP_OCLUSAOLOG=q700-760` | Igual ao `t...`, mas pelo número do quadro (o mesmo das fotos da rajada, `diag\rajada_N`): serve para cutscenes, que não têm roteiro (`tools\cutscenes.ps1 -Rajadas "4" -Extra "FCRECOMP_OCLUSAOLOG=q700-760"`) |
| `FCRECOMP_OCLUSAOSPRITE=1` | Volta ao antigo: os sprites de 4 vértices (brilho do sol, faróis) também ficavam "otimistas" como as caixas do mar (nunca 0 e com a contagem antiga do registro): farol atravessando a pedra e sol piscando na cutscene da 01_beach |
| `FCRECOMP_OCLUSAOSEMESPERA=1` | Volta ao antigo: o GetData do jogo que ouve "ainda não" respondia logo, sem pedir a contagem já pronta na placa. O jogo via o resultado 3 quadros depois da caixa de teste e o brilho do farol do jipe ficava ~6 quadros por cima da pedra (cutscene da 01_beach; no Xbox some em 1-2). `FCRECOMP_OCLUSAOESPERA2=1` (teste): espera também a caixa do quadro ainda aberto |
| `FCRECOMP_OCLUSAOTETO=ms` | Teto, por quadro do jogo, da soma das esperas pela contagem das consultas de oclusão (padrão 4 ms; 0 = sem teto, como antes). Sem teto, na selva da 02_native o jogo esperava ~21 ms por quadro (22-23 FPS); com 4 ms, 30 FPS |
| `FCRECOMP_OCLUSAOANTIGA=1` | Volta ao jeito antigo de responder as consultas de oclusão (antes da correção dos bugs 20 e 21: luz atravessando modelos e sol piscando), para comparar |
| `FCRECOMP_OCLUSAOPADRAO=1` | Desliga o item 40: consulta de oclusão nova (sem contagem da GPU ainda) volta a ser respondida na hora com "1000 amostras" em vez de ficar "ainda não pronta" até a contagem real. Com `FCRECOMP_OCLUSAOLOG=t...` as adiadas aparecem como `consulta nova ... respondida com N (adiada desde a troca ...)` |
| `FCRECOMP_SEMUCP=1` | Ignora os planos de corte do usuário (o reflexo da água volta a mostrar o fundo do mar; para comparar) |
| `FCRECOMP_ROTEIROATE=tique` | A repetição do roteiro termina nesse tique (o jogo fica ali, com o controle real). No script: `tools\repetir_roteiro.ps1 -Gravacao X -Ate 1250 -Segundos 0` |
| `MICTESTE=1` / `MICTESTE=0` no `ao_vivo.txt` | Abre / fecha o microfone como o botão TESTAR da barra de nível (janela de áudio do F2 e aba ÁUDIO do launcher). Num fone Bluetooth, o "atraso da saida" do registro `[audio] volume maximo` mostra o modo de chamada do Windows (~36 ms em vez de ~290) e a parada do som na troca (o tempo dos 375 quadros passa de 2000 ms) |
| `GPUPICO=ms` no `ao_vivo.txt` | Mede a GPU de todos os quadros; cada quadro acima de `ms` vai para `build_game\picos_gpu.txt` com os 40 desenhos mais caros e a soma por pixel shader (deixa o jogo mais lento enquanto ligado) |
| `FCRECOMP_SOMPRIORIDADENORMAL=1` | A thread do som volta à prioridade normal (sem MMCSS "Pro Audio"; para comparar estalos no som) |
| `FCRECOMP_SEMCORTE=1` | Desenha as duas faces de tudo (antes do bug 23 não descartávamos as faces de costas) |
| `FCRECOMP_ROTEIRORELOGIOREAL=1` | Repetição do roteiro com o relógio real (sem a duração gravada de cada quadro) |
| `build_game\ao_vivo.txt` | Testes com o jogo aberto: `PULARPS=`/`PULARVS=` hashes, `FOTO=arquivo.bmp`, `CAPTURA=1`, `CONSTVS=HASH [N=n]`, `TEXLOG=HASH`, `SOMBRAMIP0=1`, `SEMCORTE=1` (o jogo relê quando o arquivo muda; arquivo mais velho que a abertura do jogo é ignorado) |
| `FCRECOMP_TEXDUMP=endereco` ou `=ps=HASH` | Salva cada nível de mipmap de uma textura (ou das texturas de um pixel shader) em `build\diag\textura_*.dds` |
| `FCRECOMP_CONSTLOG=HASH` | Escreve no log as constantes do vertex shader com esse hash (3 desenhos) |
| `FCRECOMP_SEMANISO=1` | Ignora a anisotropia e o ajuste de mipmap pedidos pelas texturas (para comparar) |
| `FCRECOMP_EXPERIMENTO=4` | Depois do início da fase, alterna a cada 300 quadros entre: normal, sem consultas de oclusão, sem planos de corte, limpeza da superfície inteira; anota no log (`[exp]`) o tempo de GPU de cada um |
| `FCRECOMP_LIMPATUDO=1` | Limpa a superfície inteira nas limpezas dos resolves (antigo), em vez de só o retângulo copiado |
| `FCRECOMP_COR16=1` | Todas as superfícies de desenho em 16 bits por canal (antigo); o padrão usa 8 bits nos alvos de 8 bits do Xbox |
| `FCRECOMP_LOGLADO=0` | Não reposiciona as janelas (por padrão o log fica à esquerda e o jogo à direita) |
| `FCRECOMP_PRESENTLOG=inicio-fim` | Mostra como cada quadro foi montado na tela (quais pedaços, ou por que não foi apresentado) |

## Interruptores antigos por variável de ambiente (continuam valendo)

`FCRECOMP_PERF`, `FCRECOMP_PROFILE=inicio:duracao`, `FCRECOMP_NOLINEAR=1|HASH`, `FCRECOMP_NOMIPS`, `FCRECOMP_GPUSHOT=N|0:N`, `FCRECOMP_DRAWLOG=N`, `FCRECOMP_PSLOG=HASH`, `FCRECOMP_PSDUMP`, `FCRECOMP_VBLOG=HASH`, `FCRECOMP_SIMPLEPS`, `FCRECOMP_NOTEX`, `FCRECOMP_VSYNC`, `FCRECOMP_STATS`
