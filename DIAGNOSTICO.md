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
| `FCRECOMP_RESOLVELOG=0286E000` ou `*960-1100` | Anota cada cópia da GPU (resolve) para esses endereços, ou todas nesses quadros |
| `FCRECOMP_NOPERSIST=1` / `FCRECOMP_NOPIXSHIFT=1` | Desligam as correções da tela de carregamento e do meio pixel (para comparar) |
| `FCRECOMP_FAIXAS=1` | Volta a desenhar a tela em duas faixas como o Xbox (o padrão desenha a tela inteira de uma vez, metade do trabalho de geometria) |
| `FCRECOMP_SPINWAIT=1` | Volta às esperas em giro (thread de renderização do jogo esperando a GPU, agendador de tarefas do jogo consultando a hora e processador de comandos); o padrão agora dorme entre as verificações |
| `FCRECOMP_TEMPOLOG=1` | Mostra no log quem chama a função de hora do jogo com mais frequência |
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
| `FCRECOMP_SEMRECEITA=1` | Desliga o item 38: não aplica o `receita_shaders.txt` (ao lado do exe) no warmcache. O registro mostra `[receita] N shaders montados do shaders.dat (...), N pipelines e N superficies acrescentados`. Para aplicar de novo apague `warmcache\receita_aplicada.txt` |
| `FCRECOMP_COLETARRECEITA=1` | (o `coletar_receita.bat` liga) Grava `receitas\receita_<data>.txt` com o warmcache e cada shader/pipeline/superfície novo, no formato da receita. Registro: `[receita] coletando em ...`. Junte com `python tools\gerar_receita.py juntar <pasta>` |
| `FCRECOMP_SEMDRED=1` | Desliga o DRED (item 42; ligado por padrão): sem ele, se a placa de vídeo cair, o registro não diz até qual comando/desenho ela chegou |
| `FCRECOMP_SEMMASCARA=1` | Na troca de programa (jogo ↔ editor, troca de fase) a placa não segura a tela de carregamento enquanto o jogo novo começa escuro (item 30); a janela continua nascendo com a tela de carregamento |
| `FCRECOMP_SOMBRANAN=0` | Desliga a correção da luz do sol sem sombra válida (casas e caixas pretas na vila da fase 21_DUNEA). `=valor` muda a profundidade usada (teste) |
| `FCRECOMP_NANLOG=1` | Anota no log as constantes de shader de vértices com valor inválido (NaN) que o jogo manda para a placa e as luzes corrigidas pelo `FCRECOMP_SOMBRANAN` |
| `FCRECOMP_ROTEIROCAPTURA=etapa:tique` | Durante a repetição de um roteiro, faz a captura de diagnóstico (como F9) nesse tique |
| `FCRECOMP_GPUPROF=1` | Perfilador da GPU: mostra no log quais shaders gastam mais tempo da placa de vídeo. Junto com uma captura, grava `tempos_gpu.txt` (tempo de cada desenho) |
| `FCRECOMP_QUADROLOG=1` | Grava `build\quadros.csv`: cada quadro com a hora real (ms), quantos quadros de som o jogo já entregou e o total de desenhos. Mostra se a imagem anda no ritmo do som |
| `FCRECOMP_XMALOG=1` / `=2` | Log do chip de som XMA (duração de cada som, quadros por segundo). Com `=2`, também os primeiros bytes de cada bloco de som enviado ao chip; `python tools\xma_achar.py` acha esses blocos no `dvdsound.dat` e `python tools\xma_duracao.py` mostra a duração real no disco |
| `FCRECOMP_MOUSELOG=1` | Mostra no log cada leitura do mouse e o valor que ele virou no analógico direito |
| `FCRECOMP_SAVELOG=1` | Mostra no log cada chamada de usuário, perfil e saves (XamUser*, XamContent*, notificações). Desde o item 32 os saves ficam na pasta do perfil do launcher (`opcoes.ini perfil=`; vazio = `save\`, outro = `save\perfis\<nome>\`); o log mostra "jogos salvos: ... (perfil ...)" |
| `FCRECOMP_SAVEDIR=pasta` | Usa outra pasta de saves (o `carregar_bug.ps1` usa `build\save_teste`) |
| `FCRECOMP_LANLOG=1` / `=2` | Rede (SYSTEM LINK): anota sockets, envios e recebimentos (até 200), mensagens de sessão e `XNetXnAddrToInAddr`; com `=2` também o conteúdo dos primeiros 150 pacotes em hexadecimal |
| `PREDATOR_LAN_IP=127.0.0.2` | Teste de rede com várias cópias no mesmo PC (as outras com .3, .4): endereço próprio, broadcast para 127.0.0.1–8, registro `fcrecomp_log_lan_2.txt` |
| `PREDATOR_ONLINE_HOST=ip[:porta]` | SYSTEM LINK pela internet: procura a partida direto nesse IP (ou nome) do anfitrião, além do broadcast. É o mesmo que o campo "IP do anfitrião" do launcher (a variável vale mais). `porta` = porta de busca do anfitrião (padrão 1004) |
| `PREDATOR_ONLINE_IP_PUBLICO=ip` | IP de internet deste PC (anunciado no endereço do Xbox), em vez de perguntar ao roteador |
| `PREDATOR_UPNP=0` / `=1` | Desliga o UPnP (consulta do IP de internet e abertura automática das portas no roteador) / liga no teste no mesmo PC, onde vem desligado |
| `PREDATOR_SEMBROADCAST=1` | Não manda broadcast: a busca e o anúncio da partida só vão pelo caminho direto do IP (teste do modo internet) |
| `PREDATOR_NAT_TESTE=1` | Com `PREDATOR_LAN_IP`: finge que a cópia está atrás de um roteador (portas do jogo +300; IP local anunciado 192.168.77.x, que ninguém alcança) |
| `PredatorGame.exe --upnp-teste` | Confere o UPnP do roteador: pergunta o IP de internet, abre as portas do jogo e fecha de novo; resultado em `upnp_teste.txt` |
| `FCRECOMP_AUTOINPUT="...,M6:A,F4-20:LUP"` | Itens com `M` contam os segundos a partir do menu principal, com `F` a partir do início da fase (precisa de `FCRECOMP_MISSAO`). `inicio-fim:BOTAO` segura o botão. Analógicos: `LUP`/`LDOWN`/`LLEFT`/`LRIGHT` (andar), `RUP`/`RDOWN`/`RLEFT`/`RRIGHT` (olhar); gatilhos `LT`/`RT`. No `carregar_bug.ps1`: `-Roteiro "F4-20:LUP"` |
| `FCRECOMP_OCLUSAOLOG=1` / `FCRECOMP_OCLUSAOFIXA=1` | Mostra no log quantos pixels cada consulta de oclusão contou / volta ao antigo "sempre visível" (para comparar halos de luz e objetos que somem) |
| `FCRECOMP_OCLUSAOLOG=t4370-4445` | Igual, mas só nesses tiques de um roteiro gravado, com mais detalhes: a resposta de cada registro (e de qual faixa da tela), as consultas fechadas sem desenho e o número que o jogo vê no fim |
| `FCRECOMP_OCLUSAOANTIGA=1` | Volta ao jeito antigo de responder as consultas de oclusão (antes da correção dos bugs 20 e 21: luz atravessando modelos e sol piscando), para comparar |
| `FCRECOMP_OCLUSAOPADRAO=1` | Desliga o item 40: consulta de oclusão nova (sem contagem da GPU ainda) volta a ser respondida na hora com "1000 amostras" em vez de ficar "ainda não pronta" até a contagem real. Com `FCRECOMP_OCLUSAOLOG=t...` as adiadas aparecem como `consulta nova ... respondida com N (adiada desde a troca ...)` |
| `FCRECOMP_SEMUCP=1` | Ignora os planos de corte do usuário (o reflexo da água volta a mostrar o fundo do mar; para comparar) |
| `FCRECOMP_ROTEIROATE=tique` | A repetição do roteiro termina nesse tique (o jogo fica ali, com o controle real). No script: `tools\repetir_roteiro.ps1 -Gravacao X -Ate 1250 -Segundos 0` |
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
