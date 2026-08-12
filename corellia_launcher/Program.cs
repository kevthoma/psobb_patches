// Corellia launcher + options.
//
// Replaces the old "Launch Corellia.vbs" + "corellia_prompt.ps1" (kills the VBScript
// deprecation warning and the PowerShell console flash). A small WinForms window lets the
// player pick display mode + windowed resolution + post-processing effects + HUD scale, all
// of which are written to the widescreen wrapper's config (widescreen.cfg). Because the
// wrapper (d3d8.dll) stays loaded in both modes, largeassets/HD keep working in windowed
// mode too. On Play it runs the Tailscale reachability preflight, then launches the game.

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Windows.Forms;

namespace Corellia
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    class MainForm : Form
    {
        readonly string dir;
        readonly string cfgPath;

        RadioButton rbFull, rbWin;
        ComboBox cboRes, cboSceneSharpen;
        CheckBox cbSMAA, cbSSAO, cbCel, cbDOF, cbHDR, cbMSAA, cbSceneSharpen, cbController;

        static readonly string[] SharpenStrengths = { "0.25", "0.40", "0.50", "0.65", "0.80", "1.0" };

        // Windowed sizes offered to players (widescreen first, then 4:3 legacy).
        static readonly string[] Resolutions = {
            "1280 x 720", "1366 x 768", "1600 x 900", "1920 x 1080", "2560 x 1440",
            "3840 x 2160", "1600 x 1200", "1280 x 960", "1024 x 768"
        };

        public MainForm()
        {
            dir = AppDomain.CurrentDomain.BaseDirectory;
            cfgPath = Path.Combine(dir, "widescreen.cfg");
            BuildUI();
            TryLoadIcon();
            LoadConfig();
        }

        // ---- UI ----------------------------------------------------------------
        void BuildUI()
        {
            Text = "Corellia";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterScreen;
            MaximizeBox = false;
            MinimizeBox = false;
            ClientSize = new Size(430, 416);
            Font = new Font("Segoe UI", 9f);

            var title = new Label {
                Text = "Corellia", AutoSize = true, Location = new Point(16, 12),
                Font = new Font("Segoe UI", 15f, FontStyle.Bold)
            };
            Controls.Add(title);

            // Display
            var gDisplay = new GroupBox { Text = "Display", Location = new Point(16, 50), Size = new Size(398, 96) };
            rbFull = new RadioButton { Text = "Fullscreen (widescreen)", Location = new Point(14, 24), AutoSize = true };
            rbWin = new RadioButton { Text = "Windowed", Location = new Point(14, 54), AutoSize = true };
            cboRes = new ComboBox { Location = new Point(150, 52), Size = new Size(150, 24), DropDownStyle = ComboBoxStyle.DropDownList };
            cboRes.Items.AddRange(Resolutions);
            gDisplay.Controls.Add(rbFull);
            gDisplay.Controls.Add(rbWin);
            gDisplay.Controls.Add(cboRes);
            Controls.Add(gDisplay);
            rbWin.CheckedChanged += (s, e) => cboRes.Enabled = rbWin.Checked;

            // Effects
            var gFx = new GroupBox { Text = "Effects", Location = new Point(16, 156), Size = new Size(398, 96) };
            cbSMAA = new CheckBox { Text = "SMAA (anti-alias)", Location = new Point(14, 26), AutoSize = true };
            cbSSAO = new CheckBox { Text = "SSAO", Location = new Point(160, 26), AutoSize = true };
            cbCel  = new CheckBox { Text = "Cel shading", Location = new Point(270, 26), AutoSize = true };
            cbDOF  = new CheckBox { Text = "Depth of field", Location = new Point(14, 58), AutoSize = true };
            cbHDR  = new CheckBox { Text = "HDR", Location = new Point(160, 58), AutoSize = true };
            cbMSAA = new CheckBox { Text = "MSAA", Location = new Point(270, 58), AutoSize = true };
            foreach (var c in new Control[] { cbSMAA, cbSSAO, cbCel, cbDOF, cbHDR, cbMSAA }) gFx.Controls.Add(c);
            Controls.Add(gFx);

            // Sharpening (post-process) — crisps the 3D scene; leaves the HUD/text overlay untouched.
            var gSharp = new GroupBox { Text = "Sharpening (3D scene)", Location = new Point(16, 262), Size = new Size(398, 54) };
            cbSceneSharpen = new CheckBox { Text = "Enabled", Location = new Point(14, 24), AutoSize = true };
            var lblS1 = new Label { Text = "Strength:", Location = new Point(150, 25), AutoSize = true };
            cboSceneSharpen = new ComboBox { Location = new Point(218, 21), Size = new Size(70, 24), DropDownStyle = ComboBoxStyle.DropDownList };
            cboSceneSharpen.Items.AddRange(SharpenStrengths);
            foreach (var c in new Control[] { cbSceneSharpen, lblS1, cboSceneSharpen }) gSharp.Controls.Add(c);
            cbSceneSharpen.CheckedChanged += (s, e) => cboSceneSharpen.Enabled = cbSceneSharpen.Checked;
            Controls.Add(gSharp);

            // Controller button prompts (HD UI Controller Edition): swaps f256_hyouji.prs so on-screen
            // button hints suit a gamepad (e.g. Palette Swap shows "R" instead of "Ctrl").
            cbController = new CheckBox { Text = "Controller button prompts", Location = new Point(24, 326), AutoSize = true };
            Controls.Add(cbController);

            var btnPlay = new Button { Text = "Play", Location = new Point(150, 356), Size = new Size(130, 44) };
            btnPlay.Font = new Font("Segoe UI", 11f, FontStyle.Bold);
            btnPlay.Click += OnPlay;
            Controls.Add(btnPlay);
            AcceptButton = btnPlay;
        }

        void TryLoadIcon()
        {
            try
            {
                foreach (var name in new[] { "corellia_canary.ico", "corellia.ico", "corellia_coronet.ico" })
                {
                    var p = Path.Combine(dir, name);
                    if (File.Exists(p)) { Icon = new Icon(p); break; }
                }
            }
            catch { /* icon is cosmetic */ }
        }

        // ---- Config ------------------------------------------------------------
        Dictionary<string, string> ReadCfg()
        {
            var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (File.Exists(cfgPath))
            {
                foreach (var raw in File.ReadAllLines(cfgPath))
                {
                    var line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#' || line[0] == ';' || line[0] == '/') continue;
                    int eq = line.IndexOf('=');
                    if (eq > 0) d[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
                }
            }
            return d;
        }

        static bool AsBool(Dictionary<string, string> d, string key, bool dflt)
        {
            if (!d.TryGetValue(key, out var v)) return dflt;
            v = v.Trim().ToLowerInvariant();
            if (v.StartsWith("on") || v.StartsWith("true") || v.StartsWith("1")) return true;
            if (v.StartsWith("off") || v.StartsWith("false") || v.StartsWith("0")) return false;
            return dflt;
        }

        void LoadConfig()
        {
            var d = ReadCfg();
            cbMSAA.Checked = AsBool(d, "MSAA", true);
            cbSMAA.Checked = AsBool(d, "SMAA", true);
            cbSSAO.Checked = AsBool(d, "SSAO", true);
            cbCel.Checked  = AsBool(d, "CelShader", true);
            cbDOF.Checked  = AsBool(d, "DOF", true);
            cbHDR.Checked  = AsBool(d, "HDR", true);
            cbSceneSharpen.Checked = AsBool(d, "SceneSharpen", true);
            SelectStrength(cboSceneSharpen, d.TryGetValue("SceneSharpenStrength", out var ss) ? ss.Trim() : "0.50");
            cboSceneSharpen.Enabled = cbSceneSharpen.Checked;

            cbController.Checked = AsBool(d, "ControllerPrompts", true);

            int w = ParseInt(d, "WindowWidth", 1600);
            int h = ParseInt(d, "WindowHeight", 1200);
            string res = w + " x " + h;
            int idx = Array.IndexOf(Resolutions, res);
            if (idx < 0) { cboRes.Items.Add(res); idx = cboRes.Items.Count - 1; }
            cboRes.SelectedIndex = idx;

            bool windowed = AsBool(d, "Windowed", false);
            rbWin.Checked = windowed;
            rbFull.Checked = !windowed;
            cboRes.Enabled = windowed;
        }

        static int ParseInt(Dictionary<string, string> d, string key, int dflt)
        {
            if (d.TryGetValue(key, out var v) && int.TryParse(v.Trim(), out var i) && i > 0) return i;
            return dflt;
        }

        static void SelectStrength(ComboBox combo, string val)
        {
            int i = combo.Items.IndexOf(val);
            if (i < 0) { combo.Items.Add(val); i = combo.Items.Count - 1; }
            combo.SelectedIndex = i;
        }

        void SaveConfig()
        {
            var lines = File.Exists(cfgPath)
                ? new List<string>(File.ReadAllLines(cfgPath))
                : new List<string>();

            SetKey(lines, "MSAA", cbMSAA.Checked ? "1" : "0");
            SetKey(lines, "SMAA", cbSMAA.Checked ? "1" : "0");
            SetKey(lines, "SSAO", cbSSAO.Checked ? "1" : "0");
            SetKey(lines, "CelShader", cbCel.Checked ? "1" : "0");
            SetKey(lines, "DOF", cbDOF.Checked ? "1" : "0");
            SetKey(lines, "HDR", cbHDR.Checked ? "1" : "0");
            SetKey(lines, "SceneSharpen", cbSceneSharpen.Checked ? "1" : "0");
            SetKey(lines, "SceneSharpenStrength", (string)cboSceneSharpen.SelectedItem ?? "0.50");
            SetKey(lines, "ControllerPrompts", cbController.Checked ? "1" : "0");
            // HUD scaling is entangled with the widescreen layout math in the wrapper (non-1.0
            // leaves a seam), so it's not exposed — lock it to the value that renders correctly.
            SetKey(lines, "HUDScale", "1.0");

            SetKey(lines, "Windowed", rbWin.Checked ? "1" : "0");
            ParseRes((string)cboRes.SelectedItem, out int w, out int h);
            SetKey(lines, "WindowWidth", w.ToString());
            SetKey(lines, "WindowHeight", h.ToString());

            File.WriteAllText(cfgPath, string.Join("\r\n", lines) + "\r\n");
        }

        static void SetKey(List<string> lines, string key, string val)
        {
            for (int i = 0; i < lines.Count; i++)
            {
                var t = lines[i].TrimStart();
                int eq = t.IndexOf('=');
                if (eq > 0 && t.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                {
                    lines[i] = key + "=" + val;
                    return;
                }
            }
            lines.Add(key + "=" + val);
        }

        static void ParseRes(string s, out int w, out int h)
        {
            w = 1600; h = 1200;
            if (string.IsNullOrEmpty(s)) return;
            var parts = s.Split(new[] { 'x', 'X', '*' }, 2);
            if (parts.Length == 2 && int.TryParse(parts[0].Trim(), out var pw) && int.TryParse(parts[1].Trim(), out var ph))
            { w = pw; h = ph; }
        }

        // ---- Play / preflight --------------------------------------------------
        void OnPlay(object sender, EventArgs e)
        {
            try { SaveConfig(); }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Couldn't save settings:\n" + ex.Message, "Corellia",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }

            ApplyControllerPrompts();

            ReadServer(out string host, out int port);
            EnableTailscaleRoutes();
            if (!IsReachable(host, port, 1500))
            {
                System.Threading.Thread.Sleep(800); // let freshly-accepted routes come up
                while (!IsReachable(host, port, 1500))
                {
                    var msg =
                        "Can't reach the Corellia server (" + host + ") yet.\n\n" +
                        "1. Open Tailscale and make sure it's connected.\n" +
                        "2. Right-click the Tailscale tray icon and enable\n" +
                        "   'Use Tailscale subnet routes'.\n\n" +
                        "Then click Retry.";
                    var r = MessageBox.Show(this, msg, "Corellia - not connected yet",
                        MessageBoxButtons.RetryCancel, MessageBoxIcon.Warning);
                    if (r != DialogResult.Retry) return; // Cancel = don't launch
                    EnableTailscaleRoutes();
                }
            }

            var exe = Path.Combine(dir, "online_e.exe");
            if (!File.Exists(exe))
            {
                MessageBox.Show(this, "online_e.exe not found next to the launcher.", "Corellia",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            try
            {
                Process.Start(new ProcessStartInfo { FileName = exe, WorkingDirectory = dir, UseShellExecute = true });
                Close();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Couldn't launch the game:\n" + ex.Message, "Corellia",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        // Swap the active HUD button-prompt texture (data/f256_hyouji.prs) to the keyboard or
        // controller variant kept in ui_variants/. Idempotent; no-op if the variants aren't present.
        void ApplyControllerPrompts()
        {
            try
            {
                string src = Path.Combine(dir, "ui_variants",
                    cbController.Checked ? "f256_hyouji.controller.prs" : "f256_hyouji.keyboard.prs");
                string dst = Path.Combine(dir, "data", "f256_hyouji.prs");
                if (File.Exists(src) && File.Exists(dst)) File.Copy(src, dst, true);
            }
            catch { /* non-fatal: fall back to whatever's already in data/ */ }
        }

        void ReadServer(out string host, out int port)
        {
            host = "192.168.1.15"; port = 11000;
            var p = Path.Combine(dir, "psobb.cfg");
            if (!File.Exists(p)) return;
            foreach (var raw in File.ReadAllLines(p))
            {
                var line = raw.Trim();
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                var k = line.Substring(0, eq).Trim();
                var v = line.Substring(eq + 1).Trim();
                if (k.Equals("PatchHost", StringComparison.OrdinalIgnoreCase) && v.Length > 0) host = v;
                else if (k.Equals("PatchPort", StringComparison.OrdinalIgnoreCase) && int.TryParse(v, out var pp)) port = pp;
            }
        }

        static void EnableTailscaleRoutes()
        {
            try
            {
                string ts = "tailscale.exe";
                var pf = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Tailscale", "tailscale.exe");
                if (File.Exists(pf)) ts = pf;
                var psi = new ProcessStartInfo
                {
                    FileName = ts,
                    Arguments = "set --accept-routes=true",
                    UseShellExecute = false,
                    CreateNoWindow = true
                };
                var proc = Process.Start(psi);
                if (proc != null) proc.WaitForExit(4000);
            }
            catch { /* best-effort; not fatal on the LAN */ }
        }

        static bool IsReachable(string host, int port, int timeoutMs)
        {
            using (var client = new TcpClient())
            {
                try
                {
                    var iar = client.BeginConnect(host, port, null, null);
                    return iar.AsyncWaitHandle.WaitOne(timeoutMs) && client.Connected;
                }
                catch { return false; }
            }
        }
    }
}
