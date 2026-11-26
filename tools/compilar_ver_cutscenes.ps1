# Compila build_game\VerCutscenes.exe: uma janela para escolher qualquer cutscene (videos Bink e cenas das fases) e
# abrir o jogo direto nela (o mesmo que tools\cutscenes.ps1: FCRECOMP_VIDEO / FCRECOMP_CENA, runtime\cena_direta.cpp).
# Usa o compilador C# que vem no Windows (.NET Framework 4); a lista de cenas (tools\cutscenes.txt, gerada por
# tools\gerar_cutscenes.py) vai dentro do exe. O exe tem de ficar na pasta do PredatorGame.exe.
# Uso: powershell -ExecutionPolicy Bypass -File tools\compilar_ver_cutscenes.ps1
$ErrorActionPreference = "Stop"
$raiz = Split-Path $PSScriptRoot
$saida = "$raiz\PredatorRecomp\build_game\VerCutscenes.exe"
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { "nao achei o compilador C# do Windows ($csc)"; exit 1 }
$lista = "$PSScriptRoot\cutscenes.txt"
if (-not (Test-Path $lista)) { "falta tools\cutscenes.txt: rode python tools\gerar_cutscenes.py"; exit 1 }

$fonte = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

class Item
{
    public string Video, Fase, Gatilho, Evento, Ancora;
}

class VerCutscenes : Form
{
    static readonly Color FUNDO = Color.FromArgb(22, 20, 18), CARTAO = Color.FromArgb(34, 31, 27),
                          TEXTO = Color.FromArgb(232, 226, 214), FRACO = Color.FromArgb(150, 142, 128),
                          AMBAR = Color.FromArgb(232, 168, 48), ERRO = Color.FromArgb(220, 80, 60);
    readonly string pasta = AppDomain.CurrentDomain.BaseDirectory;
    readonly ListView lista = new ListView();
    readonly Button ver = new Button(), fechar = new Button();
    readonly CheckBox levar = new CheckBox();
    readonly Label situacao = new Label();
    readonly System.Windows.Forms.Timer relogio = new System.Windows.Forms.Timer();
    Process jogo;
    long lidos;

    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [System.Runtime.InteropServices.DllImport("uxtheme.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern int SetWindowTheme(IntPtr h, string app, string ids);
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);

    [STAThread]
    static void Main()
    {
        try { SetProcessDPIAware(); } catch { }   // nitido com a escala do Windows (o tamanho e ajustado abaixo)
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new VerCutscenes());
    }

    VerCutscenes()
    {
        Text = "Ver cutscenes - Far Cry Instincts Predator Recomp";
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
        AutoScaleMode = AutoScaleMode.None;   // tudo e escrito em 96 dpi e escalado no fim (Scale)
        BackColor = FUNDO; ForeColor = TEXTO;
        Font = new Font("Segoe UI", 10f);
        ClientSize = new Size(820, 640);
        StartPosition = FormStartPosition.CenterScreen;

        var titulo = new Label { Text = "CUTSCENES", ForeColor = AMBAR, Font = new Font("Segoe UI Semibold", 13f), AutoSize = true, Location = new Point(16, 12) };
        var dica = new Label { Text = "Escolha um vídeo ou uma cena e clique em VER (ou dê dois cliques). O jogo abre direto nela, sem passar pelos menus.\r\n" +
                                      "As cenas das fases usam saves separados (pasta save_cena): seus jogos salvos não são tocados.",
                               ForeColor = FRACO, AutoSize = false, Location = new Point(16, 40), Size = new Size(788, 40),
                               Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };

        lista.View = View.Details; lista.FullRowSelect = true; lista.MultiSelect = false; lista.HideSelection = false;
        lista.BackColor = CARTAO; lista.ForeColor = TEXTO; lista.BorderStyle = BorderStyle.None;
        lista.HeaderStyle = ColumnHeaderStyle.None;
        lista.Location = new Point(16, 112); lista.Size = new Size(788, 422);
        var cab = new Panel { Location = new Point(16, 86), Size = new Size(788, 26), BackColor = Color.FromArgb(44, 40, 35),
                              Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
        int x = 0;
        foreach (var c in new[] { new { t = "FASE / VÍDEO", w = 230 }, new { t = "GATILHO", w = 260 }, new { t = "CENA (ÂNCORA)", w = 270 } })
        {
            cab.Controls.Add(new Label { Text = c.t, ForeColor = AMBAR, Font = new Font("Segoe UI Semibold", 9f), AutoSize = false,
                                         Location = new Point(x + 4, 4), Size = new Size(c.w - 4, 20) });
            x += c.w;
        }
        lista.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        lista.Columns.Add("Fase / vídeo", 230);
        lista.Columns.Add("Gatilho", 260);
        lista.Columns.Add("Cena (âncora)", 270);
        lista.DoubleClick += delegate { Ver(); };
        lista.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { e.Handled = true; Ver(); } };   // Enter = VER
        // tema escuro do Windows na lista (barras de rolagem e destaque da linha)
        lista.HandleCreated += delegate { try { SetWindowTheme(lista.Handle, "DarkMode_Explorer", null); } catch { } };
        lista.SelectedIndexChanged += delegate { AtualizarBotoes(); };

        levar.Text = "Levar o jogador até o gatilho da cena"; levar.Checked = true; levar.AutoSize = true;
        levar.Location = new Point(16, 546); levar.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;

        situacao.AutoSize = false; situacao.Location = new Point(16, 576); situacao.Size = new Size(520, 50);
        situacao.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right; situacao.ForeColor = FRACO;

        Botao(fechar, "FECHAR JOGO", new Point(548, 584)); fechar.Click += delegate { FecharJogo(); };
        Botao(ver, "VER", new Point(684, 584)); ver.Click += delegate { Ver(); };
        ver.BackColor = Color.FromArgb(150, 30, 22);

        Controls.AddRange(new Control[] { titulo, dica, cab, lista, levar, situacao, fechar, ver });
        Carregar();
        // escala da tela (125%, 150%...): posicoes, tamanhos e colunas (as fontes ja seguem a escala)
        float escala;
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) escala = g.DpiX / 96f;
        Scale(new SizeF(escala, escala));
        foreach (ColumnHeader c in lista.Columns) c.Width = (int)(c.Width * escala);
        MinimumSize = new Size((int)(640 * escala), (int)(420 * escala));
        relogio.Interval = 400;
        relogio.Tick += delegate { LerRegistro(); };
        relogio.Start();
        AtualizarBotoes();
        Mostrar("Pronto.", FRACO);
    }

    void Botao(Button b, string texto, Point onde)
    {
        b.Text = texto; b.Location = onde; b.Size = new Size(126, 38);
        b.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
        b.FlatStyle = FlatStyle.Flat; b.FlatAppearance.BorderColor = Color.FromArgb(90, 80, 64);
        b.BackColor = CARTAO; b.ForeColor = TEXTO; b.Font = new Font("Segoe UI Semibold", 10f);
    }

    void Mostrar(string texto, Color cor) { situacao.Text = texto; situacao.ForeColor = cor; }

    bool JogoAberto() { return jogo != null && !jogo.HasExited; }

    void AtualizarBotoes()
    {
        ver.Enabled = lista.SelectedItems.Count > 0 && !JogoAberto();
        fechar.Enabled = JogoAberto();
    }

    string PastaDoJogo()
    {
        try
        {
            foreach (string l in File.ReadAllLines(Path.Combine(pasta, "opcoes.ini")))
            {
                string t = l.Trim();
                if (t.StartsWith("pasta_jogo") && t.Contains("=")) return t.Substring(t.IndexOf('=') + 1).Trim();
            }
        }
        catch { }
        return "";
    }

    void Carregar()
    {
        string faseAntes = "";
        bool par = false;
        // videos Bink da pasta do jogo
        string videos = Path.Combine(PastaDoJogo(), "videos");
        if (Directory.Exists(videos))
        {
            var nomes = Directory.GetFiles(videos, "*.bik");
            Array.Sort(nomes);
            foreach (string f in nomes)
            {
                var it = new ListViewItem(new[] { Path.GetFileName(f), "", "vídeo" }, 0);
                it.Tag = new Item { Video = Path.GetFileName(f) };
                lista.Items.Add(it);
            }
        }
        // cenas do motor (tools\cutscenes.txt, dentro do exe)
        using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("cutscenes.txt"))
        using (var r = new StreamReader(s, Encoding.UTF8))
        {
            string l;
            while ((l = r.ReadLine()) != null)
            {
                if (l.Length == 0 || l.StartsWith("#")) continue;
                string[] c = l.Split('|');
                if (c.Length < 4) continue;
                var item = new Item { Fase = c[0], Gatilho = c[1], Evento = c[2], Ancora = c[3] };
                string gat = c[1].Length == 0 ? "(começa com a fase)" : c[1] + (c[2].Length > 0 && c[2] != "Enter" ? " (" + c[2] + ")" : "");
                var it = new ListViewItem(new[] { c[0], gat, c[3] });
                it.Tag = item;
                // cada fase com um fundo levemente diferente da anterior (no lugar dos grupos)
                if (c[0] != faseAntes) { par = !par; faseAntes = c[0]; }
                if (par) it.BackColor = Color.FromArgb(40, 36, 31);
                lista.Items.Add(it);
            }
        }
    }

    void Ver()
    {
        if (lista.SelectedItems.Count == 0) return;
        if (JogoAberto()) { Mostrar("O jogo ainda está aberto: clique em FECHAR JOGO antes de ver outra cena.", AMBAR); return; }
        if (Process.GetProcessesByName("PredatorGame").Length > 0 || Process.GetProcessesByName("PredatorEditor").Length > 0)
        {
            MessageBox.Show(this, "O jogo já está aberto. Feche-o antes de ver uma cutscene.", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        string exe = Path.Combine(pasta, "PredatorGame.exe");
        if (!File.Exists(exe))
        {
            MessageBox.Show(this, "Não achei o PredatorGame.exe nesta pasta. O VerCutscenes.exe precisa ficar junto dele.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        var item = (Item)lista.SelectedItems[0].Tag;
        var psi = new ProcessStartInfo(exe) { WorkingDirectory = pasta, UseShellExecute = false };
        // saves separados, apagados a cada vez
        string saves = Path.Combine(pasta, "save_cena");
        try { if (Directory.Exists(saves)) Directory.Delete(saves, true); Directory.CreateDirectory(saves); }
        catch (Exception e) { Mostrar("Não deu para preparar a pasta save_cena: " + e.Message, ERRO); return; }
        psi.EnvironmentVariables["FCRECOMP_SAVEDIR"] = saves;
        string nome;
        if (item.Video != null)
        {
            psi.EnvironmentVariables["FCRECOMP_VIDEO"] = item.Video;
            nome = "vídeo " + item.Video;
        }
        else
        {
            string cena = item.Fase;
            if (item.Gatilho.Length > 0) cena += ":" + item.Gatilho + (item.Evento.Length > 0 && item.Evento != "Enter" ? ":" + item.Evento : "");
            psi.EnvironmentVariables["FCRECOMP_CENA"] = cena;
            if (!levar.Checked) psi.EnvironmentVariables["FCRECOMP_CENA_LEVAR"] = "0";
            nome = "cena " + item.Ancora + " (" + item.Fase + ")";
        }
        try { File.Delete(Path.Combine(pasta, "fcrecomp_log.txt")); } catch { }
        lidos = 0;
        try { jogo = Process.Start(psi); }
        catch (Exception e) { Mostrar("O jogo não abriu: " + e.Message, ERRO); return; }
        jogo.EnableRaisingEvents = true;
        jogo.Exited += delegate { BeginInvoke(new Action(delegate { Mostrar("Jogo fechado.", FRACO); AtualizarBotoes(); })); };
        Mostrar("Abrindo o jogo para a " + nome + "...", AMBAR);
        AtualizarBotoes();
    }

    void FecharJogo()
    {
        if (!JogoAberto()) return;
        try { EventWaitHandle.OpenExisting("FCRecomp_Sair").Set(); }
        catch { try { jogo.Kill(); } catch { } }
        Mostrar("Fechando o jogo...", FRACO);
    }

    // o que o jogo anota (fcrecomp_log.txt) vira o texto da situacao
    void LerRegistro()
    {
        if (!JogoAberto()) return;
        string log = Path.Combine(pasta, "fcrecomp_log.txt");
        try
        {
            using (var f = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                if (f.Length < lidos) lidos = 0;
                f.Seek(lidos, SeekOrigin.Begin);
                var r = new StreamReader(f, Encoding.UTF8);
                string resto = r.ReadToEnd();
                int fim = resto.LastIndexOf('\n');
                if (fim < 0) return;
                lidos += Encoding.UTF8.GetByteCount(resto.Substring(0, fim + 1));
                foreach (string l in resto.Substring(0, fim).Split('\n')) Linha(l.TrimEnd('\r'));
            }
        }
        catch { }
    }

    void Linha(string l)
    {
        if (l.StartsWith("[CRASH]")) { Mostrar("O jogo travou (detalhes em fcrecomp_log.txt).", ERRO); return; }
        if (!l.StartsWith("[cena]")) return;
        if (l.Contains("video ")) Mostrar("Vídeo trocado: tocando agora.", AMBAR);
        else if (l.Contains("comando do jogo")) Mostrar("Abrindo a fase (leva uns 30 segundos)...", AMBAR);
        else if (l.Contains("jogador na fase")) Mostrar("Fase carregada; preparando a cena...", AMBAR);
        else if (l.Contains("esperando ela acabar")) Mostrar("A fase começou com outra cena. A escolhida vem quando ela acabar...", AMBAR);
        else if (l.Contains("ja esta tocando") || l.Contains("sem gatilho")) Mostrar("A cena começou junto com a fase.", TEXTO);
        else if (l.Contains("disparado")) Mostrar("Cena disparada. Se nada acontecer, essa cena não dá para abrir direto (ex.: jogador dentro de um veículo).", TEXTO);
        else if (l.Contains("invalidos")) Mostrar("Nome de cena inválido.", ERRO);
    }
}
'@

$tmp = Join-Path $env:TEMP "VerCutscenes_fonte"
New-Item -ItemType Directory -Force $tmp | Out-Null
[IO.File]::WriteAllText("$tmp\VerCutscenes.cs", $fonte, (New-Object Text.UTF8Encoding $true))
Copy-Item $lista "$tmp\cutscenes.txt" -Force
$icone = "$raiz\PredatorRecomp\runtime\recursos\predator.ico"
& $csc /nologo /target:winexe /optimize+ /codepage:65001 "/out:$saida" "/win32icon:$icone" "/resource:$tmp\cutscenes.txt,cutscenes.txt" `
    /r:System.Windows.Forms.dll /r:System.Drawing.dll "$tmp\VerCutscenes.cs"
if ($LASTEXITCODE -ne 0) { "falhou"; exit 1 }
"OK: $saida"
