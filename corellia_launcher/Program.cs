// Corellia launcher + options — one binary, two roles chosen by its own filename:
//
//   * Corellia.exe            -> LAUNCHER: runs the Tailscale reachability preflight, applies the
//                               controller-prompt texture per the saved setting, then opens online_e.exe
//                               (the PSO launcher). No settings window.
//   * option_e_corellia.exe   -> OPTIONS MENU: the settings window (Display / Effects / Sharpening /
//                               Controller prompts) with a "Save & Close" button. This is what the PSO
//                               launcher's "Option" button opens (online_e.exe is patched to point at it).
//
// All settings are written to the widescreen wrapper's config (widescreen.cfg); the wrapper reads it when
// the game starts. Replaces the old Launch Corellia.vbs + corellia_prompt.ps1 (no VBScript, no console flash).

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Windows.Forms;
using Microsoft.Win32;

namespace Corellia
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            string name = "";
            try { name = Path.GetFileNameWithoutExtension(Application.ExecutablePath) ?? ""; }
            catch { }

            if (name.StartsWith("option", StringComparison.OrdinalIgnoreCase))
                Application.Run(new MainForm());   // options menu (Save & Close)
            else
                Launcher.Run();                    // preflight + apply prompts + launch, no window
        }
    }

    // ---- Shared helpers -------------------------------------------------------------
    static class Helpers
    {
        // Swap the active HUD button-prompt texture (data/f256_hyouji.prs) to the keyboard or controller
        // variant kept in ui_variants/. Idempotent; no-op if the variants aren't present.
        public static void ApplyControllerPrompts(string dir, bool controller)
        {
            try
            {
                string src = Path.Combine(dir, "ui_variants",
                    controller ? "f256_hyouji.controller.prs" : "f256_hyouji.keyboard.prs");
                string dst = Path.Combine(dir, "data", "f256_hyouji.prs");
                if (File.Exists(src) && File.Exists(dst)) File.Copy(src, dst, true);
            }
            catch { /* non-fatal: fall back to whatever's already in data/ */ }
        }

        // Read a bool key from widescreen.cfg (used by the launcher, which has no UI).
        public static bool ReadCfgBool(string dir, string key, bool dflt)
        {
            try
            {
                var p = Path.Combine(dir, "widescreen.cfg");
                if (!File.Exists(p)) return dflt;
                foreach (var raw in File.ReadAllLines(p))
                {
                    var line = raw.Trim();
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    if (!line.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) continue;
                    var v = line.Substring(eq + 1).Trim().ToLowerInvariant();
                    if (v.StartsWith("on") || v.StartsWith("true") || v.StartsWith("1")) return true;
                    if (v.StartsWith("off") || v.StartsWith("false") || v.StartsWith("0")) return false;
                }
            }
            catch { }
            return dflt;
        }

        public static void ReadServer(string dir, out string host, out int port)
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

        public static void EnableTailscaleRoutes()
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

        public static bool IsReachable(string host, int port, int timeoutMs)
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

        // Tailscale preflight applies only to LAN/Tailscale targets (canary). A public hostname or IP
        // (prod, e.g. corellia.thekevops.net) skips it and shows a generic connectivity message instead.
        public static bool IsPrivateHost(string host)
        {
            if (System.Net.IPAddress.TryParse(host, out var ip))
            {
                var b = ip.GetAddressBytes();
                if (b.Length == 4)
                {
                    if (b[0] == 10) return true;                               // 10.0.0.0/8
                    if (b[0] == 192 && b[1] == 168) return true;              // 192.168.0.0/16
                    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true; // 172.16.0.0/12
                    if (b[0] == 100 && b[1] >= 64 && b[1] <= 127) return true;// 100.64.0.0/10 (CGNAT/Tailscale)
                    if (b[0] == 127) return true;                             // loopback
                }
                return false; // public IP -> prod
            }
            return false; // hostname -> prod (public)
        }

        // Re-open the PSO launcher after the options menu closes. online_e.exe hands off to the Option
        // tool (and exits), so we bring it back; skipped if it's somehow still running (no duplicate).
        public static void RelaunchOnline(string dir)
        {
            try
            {
                if (Process.GetProcessesByName("online_e").Length > 0) return;
                var exe = Path.Combine(dir, "online_e.exe");
                if (File.Exists(exe))
                    Process.Start(new ProcessStartInfo { FileName = exe, WorkingDirectory = dir, UseShellExecute = true });
            }
            catch { }
        }
    }

    // ---- Launcher role (no window) --------------------------------------------------
    static class Launcher
    {
        public static void Run()
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;

            // Apply the controller-prompt texture (from the saved setting) before the game starts.
            Helpers.ApplyControllerPrompts(dir, Helpers.ReadCfgBool(dir, "ControllerPrompts", true));

            Helpers.ReadServer(dir, out string host, out int port);
            bool useTailscale = Helpers.IsPrivateHost(host); // LAN/Tailscale target (canary) vs public prod host
            if (useTailscale) Helpers.EnableTailscaleRoutes();
            if (!Helpers.IsReachable(host, port, 1500))
            {
                System.Threading.Thread.Sleep(800); // let freshly-accepted routes come up
                while (!Helpers.IsReachable(host, port, 1500))
                {
                    string msg = useTailscale
                        ? "Can't reach the Corellia server (" + host + ") yet.\n\n" +
                          "1. Open Tailscale and make sure it's connected.\n" +
                          "2. Right-click the Tailscale tray icon and enable\n" +
                          "   'Use Tailscale subnet routes'.\n\n" +
                          "Then click Retry."
                        : "Can't reach the Corellia server (" + host + ").\n\n" +
                          "Check your internet connection and try again.\n" +
                          "(If this keeps happening, the server may be down.)";
                    var r = MessageBox.Show(msg,
                        useTailscale ? "Corellia - not connected yet" : "Corellia - can't connect",
                        MessageBoxButtons.RetryCancel, MessageBoxIcon.Warning);
                    if (r != DialogResult.Retry) return; // Cancel = don't launch
                    if (useTailscale) Helpers.EnableTailscaleRoutes();
                }
            }

            var exe = Path.Combine(dir, "online_e.exe");
            if (!File.Exists(exe))
            {
                MessageBox.Show("online_e.exe not found next to the launcher.", "Corellia",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            try { Process.Start(new ProcessStartInfo { FileName = exe, WorkingDirectory = dir, UseShellExecute = true }); }
            catch (Exception ex)
            {
                MessageBox.Show("Couldn't launch the game:\n" + ex.Message, "Corellia",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }

    // ---- Options menu (window) ------------------------------------------------------
    class MainForm : Form
    {
        readonly string dir;
        readonly string cfgPath;

        RadioButton rbFull, rbWin;
        ComboBox cboRes, cboSceneSharpen;
        CheckBox cbSMAA, cbSSAO, cbCel, cbDOF, cbHDR, cbMSAA, cbSceneSharpen, cbController, cbSaveLogin;

        // The game stores login under HKCU\Software\SonicTeam\PSOBB; these DWORD flags are what the
        // native "Save ID and Password" option toggled (remember game ID / remember password).
        const string PsoRegPath = @"Software\SonicTeam\PSOBB";

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
            Text = "Corellia - Options";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterScreen;
            MaximizeBox = false;
            MinimizeBox = false;
            ClientSize = new Size(430, 436);
            Font = new Font("Segoe UI", 9f);

            var title = new Label {
                Text = "Corellia Options", AutoSize = true, Location = new Point(16, 12),
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
            cbController = new CheckBox { Text = "Controller button prompts", Location = new Point(24, 322), AutoSize = true };
            Controls.Add(cbController);

            // Remember login — toggles the game's own ACCOUNT_CHECK / PASSWORD_CHECK registry flags
            // (like the native option). We never read or write the credentials themselves.
            cbSaveLogin = new CheckBox { Text = "Save ID and Password", Location = new Point(24, 346), AutoSize = true };
            Controls.Add(cbSaveLogin);

            var btnSave = new Button { Text = "Save && Close", Location = new Point(150, 376), Size = new Size(130, 44) };
            btnSave.Font = new Font("Segoe UI", 11f, FontStyle.Bold);
            btnSave.Click += OnSaveClose;
            Controls.Add(btnSave);
            AcceptButton = btnSave;
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
            SelectStrength(cboSceneSharpen, d.TryGetValue("SceneSharpenStrength", out var ss) ? ss.Trim() : "0.25");
            cboSceneSharpen.Enabled = cbSceneSharpen.Checked;

            cbController.Checked = AsBool(d, "ControllerPrompts", true);
            cbSaveLogin.Checked = ReadSaveLogin();

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
            SetKey(lines, "SceneSharpenStrength", (string)cboSceneSharpen.SelectedItem ?? "0.25");
            SetKey(lines, "ControllerPrompts", cbController.Checked ? "1" : "0");
            // HUD scaling is entangled with the widescreen layout math in the wrapper (non-1.0
            // leaves a seam), so it's not exposed — lock it to the value that renders correctly.
            SetKey(lines, "HUDScale", "1.0");

            // INVARIANT: windowed mode is a widescreen.cfg FLAG the wrapper reads. It must never go
            // back to the old approach of renaming d3d8.dll to d3d8.dll.off — that DLL is also the
            // ASI loader, so disabling it silently drops patches/largeassets.asi, the engine then
            // reads a bundled HD .bml into a vanilla-sized buffer, and the client access-violates on
            // the first over-cap area (reliably Caves 1). The wrapper stays loaded in every mode.
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

        // ---- Save & Close ------------------------------------------------------
        void OnSaveClose(object sender, EventArgs e)
        {
            try { SaveConfig(); }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Couldn't save settings:\n" + ex.Message, "Corellia",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            // Apply the controller-prompt texture now so it takes effect for this session's launch too.
            Helpers.ApplyControllerPrompts(dir, cbController.Checked);
            WriteSaveLogin(cbSaveLogin.Checked);
            // Return to the PSO launcher (online_e.exe hands off to Option and exits).
            Helpers.RelaunchOnline(dir);
            Close();
        }

        // ---- "Save ID and Password" (game's remember-login registry flags) -----
        static bool ReadSaveLogin()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(PsoRegPath))
                {
                    if (k != null && k.GetValue("ACCOUNT_CHECK") is int i) return i != 0;
                }
            }
            catch { }
            return true; // default: remember (matches the usual out-of-box behavior)
        }

        static void WriteSaveLogin(bool on)
        {
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(PsoRegPath))
                {
                    if (k == null) return;
                    k.SetValue("ACCOUNT_CHECK", on ? 1 : 0, RegistryValueKind.DWord);
                    k.SetValue("PASSWORD_CHECK", on ? 1 : 0, RegistryValueKind.DWord);
                }
            }
            catch { }
        }
    }
}
