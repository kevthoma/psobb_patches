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
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using System.Drawing.Text;
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

        // Put the newer audio proxy in place, if the patch server has delivered one.
        //
        // WHY THIS EXISTS AT ALL. The proxy is the one file the patch server cannot update.
        // Overwriting an existing dsound.dll fails -- the client re-downloads it every login and
        // never starts. That is not a guess: it was reproduced deliberately twice, on 2026-08-27
        // with a test build and again on 2026-08-30 with the real one. CREATING a file the client
        // does not have works fine, and so does overwriting any .exe (Corellia.exe included --
        // also verified on the canary, which is what makes this code reachable at all).
        //
        // So the server ships the new proxy under a name nothing loads, dsound_v2.dll, and the
        // LAUNCHER installs it here: the game is not running yet and nothing has dsound.dll open.
        //
        // Two decisions worth keeping:
        //
        //   * REPAIR ONLY, NEVER INTRODUCE. If dsound.dll is absent we return without doing
        //     anything, because absent means the installer deliberately left it out. That is the
        //     Steam Deck: the proxy crashed the game under Proton (two crashes, 2026-08-27), so
        //     compose-client.sh excludes it. Copying it in here would reintroduce exactly that.
        //
        //   * dsound_v2.dll IS NOT DELETED afterwards. It sits in the patch index, so deleting it
        //     only makes the server re-send 132 KB on every login, forever. Left alone, the byte
        //     compare below turns this into a no-op from the second launch onward.
        public static void InstallProxyUpdate(string dir)
        {
            try
            {
                string src = Path.Combine(dir, "dsound_v2.dll");
                string dst = Path.Combine(dir, "dsound.dll");

                if (!File.Exists(src)) return;              // nothing delivered
                if (!File.Exists(dst)) return;              // repair only -- see above
                if (FilesAreIdentical(src, dst)) return;    // already applied

                File.Copy(src, dst, true);
            }
            catch
            {
                // Non-fatal on purpose. If the copy fails (someone already has the game running,
                // so the DLL is mapped), the player keeps the older proxy and we try again next
                // launch. Audio settings are never worth blocking a launch over.
            }
        }

        // Byte compare, not size-or-timestamp. The patch server writes its own mtimes, and two
        // builds of the proxy can share a size while differing in content.
        static bool FilesAreIdentical(string a, string b)
        {
            try
            {
                if (new FileInfo(a).Length != new FileInfo(b).Length) return false;
                byte[] ba = File.ReadAllBytes(a);
                byte[] bb = File.ReadAllBytes(b);
                for (int i = 0; i < ba.Length; i++)
                    if (ba[i] != bb[i]) return false;
                return true;
            }
            catch { return false; }
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

        // ---- Per-instance registry key ------------------------------------------
        // Every PSOBB-derived client -- Coronet, Agrilat, Kor Vella AND Ephinea -- historically
        // shared HKCU\Software\SonicTeam\PSOBB, so whichever ran last won. When another client
        // left behind a GRAPHICCTRL display-mode index this build can't satisfy, psobb.exe dies
        // at D3D init with "Can't find DisplayMode" / "Can't use format [D3DFMT_X8R8G8B8]".
        //
        // patch_regkey.ps1 rewrites the 5-char leaf inside the native binaries (psobb.exe,
        // online_e.exe, option_e.exe) and records the same value here as RegKey=. We read it so
        // the managed side stays in sync with what was patched. No RegKey => unpatched build =>
        // legacy shared key, which keeps old installs working unchanged.
        public const string LegacyLeaf = "PSOBB";

        // Read a string key from psobb.cfg (ReadCfgBool reads widescreen.cfg -- different file).
        public static string ReadCfgString(string dir, string key, string dflt)
        {
            try
            {
                var p = Path.Combine(dir, "psobb.cfg");
                if (!File.Exists(p)) return dflt;
                foreach (var raw in File.ReadAllLines(p))
                {
                    var line = raw.Trim();
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    if (!line.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) continue;
                    var v = line.Substring(eq + 1).Trim();
                    if (v.Length > 0) return v;
                }
            }
            catch { }
            return dflt;
        }

        public static string RegLeaf(string dir)
        {
            var leaf = ReadCfgString(dir, "RegKey", LegacyLeaf);
            // The patch is equal-length by design; anything else means a hand-edited cfg, so fall
            // back to the legacy key rather than inventing one the binaries don't know about.
            if (leaf.Length != LegacyLeaf.Length) return LegacyLeaf;
            foreach (var c in leaf)
                if (!char.IsLetterOrDigit(c) && c != '_') return LegacyLeaf;
            return leaf;
        }

        public static string PsoRegPath(string dir)
        {
            return @"Software\SonicTeam\" + RegLeaf(dir);
        }

        // Carried over from the old shared key on first run of a patched build. This is an
        // ALLOW-LIST on purpose:
        //   * GRAPHICCTRL and WINDOW_MODE are deliberately absent -- a bad display-mode index in
        //     those two is the exact failure this whole change exists to prevent, and install.reg
        //     has already seeded known-good values. Players get Corellia's defaults once.
        //   * Instance-scoped values (BILLING_SITE, OFFICIAL_SITE, CLIENT_CODE, FONT_JPN, EXT0...)
        //     are install.reg's to own and differ per build, so they are not copied either.
        static readonly string[] MigratedValues = {
            "ACCOUNT", "PASSWORD", "ACCOUNT_CTRL", "ACCOUNT_CHECK", "PASSWORD_CHECK",
            "CTRLBUF", "SOUNDCTRL", "FOCUS_SOUND", "WORD_WRAP"
        };

        const string MigrationMarker = "CORELLIA_MIGRATED";

        // One-time copy of the player's login + input/audio prefs from the old shared key into
        // this build's own key, so upgrading doesn't look like a wiped profile.
        //
        // Guarded by a marker VALUE, not by key existence: install.reg creates the new key at
        // install time, so "key is missing" is never true and would skip the copy entirely.
        //
        // Never writes back to PSOBB. Ephinea's settings must come out of this untouched.
        public static void MigrateLegacyRegistry(string dir)
        {
            try
            {
                string leaf = RegLeaf(dir);
                if (leaf.Equals(LegacyLeaf, StringComparison.OrdinalIgnoreCase)) return; // unpatched build

                using (var dst = Registry.CurrentUser.CreateSubKey(@"Software\SonicTeam\" + leaf))
                {
                    if (dst == null) return;
                    if (dst.GetValue(MigrationMarker) != null) return; // already migrated

                    using (var src = Registry.CurrentUser.OpenSubKey(@"Software\SonicTeam\" + LegacyLeaf, false))
                    {
                        if (src != null)
                        {
                            foreach (var name in MigratedValues)
                            {
                                var v = src.GetValue(name, null);
                                if (v == null) continue;
                                dst.SetValue(name, v, src.GetValueKind(name));
                            }
                        }
                    }

                    // Stamp even when there was nothing to copy, so a fresh install never re-checks.
                    dst.SetValue(MigrationMarker, 1, RegistryValueKind.DWord);
                }
            }
            catch { /* non-fatal: worst case the player re-enters their login once */ }
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

            // Give this build its own registry key a chance to inherit the player's login from
            // the old shared one. One-shot and self-marking; safe to call on every launch.
            Helpers.MigrateLegacyRegistry(dir);

            // Install a newer audio proxy if the patch server delivered one. Deliberately in the
            // LAUNCHER path only, never in the options window: the options window is opened BY the
            // PSO launcher and can be up while the game is running, which is exactly when
            // dsound.dll is mapped and cannot be replaced.
            Helpers.InstallProxyUpdate(dir);

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

        ComboBox cboMode, cboRes, cboSceneSharpen, cboFont;

        // Which modes the dropdown currently OFFERS, as indices into DisplayModes/DisplayModeKeys.
        // The dropdown position is no longer the mode index, because Fullscreen is not offered --
        // see BuildModeList. Never index DisplayModeKeys with cboMode.SelectedIndex directly.
        readonly List<int> offeredModes = new List<int>();
        CheckBox cbSMAA, cbSSAO, cbCel, cbDOF, cbHDR, cbMSAA, cbSceneSharpen, cbController, cbSaveLogin;
        TrackBar tbMaster, tbMusic, tbEffects;

        // The game stores login under HKCU\Software\SonicTeam\PSOBB; these DWORD flags are what the
        // native "Save ID and Password" option toggled (remember game ID / remember password).
        // Per-instance now -- see Helpers.PsoRegPath. Was hardcoded to the shared
        // @"Software\SonicTeam\PSOBB" before each build got its own leaf.
        static readonly string PsoRegPath = Helpers.PsoRegPath(AppDomain.CurrentDomain.BaseDirectory);

        static readonly string[] SharpenStrengths = { "0.25", "0.40", "0.50", "0.65", "0.80", "1.0" };

        // Display modes, in the order players see them: the recommended one first, the niche one
        // last. The index maps to the DisplayMode= value the wrapper parses (DISPLAY_* in Options.c).
        // Fullscreen is deliberately NOT offered any more. It is the only display mode that mutates
        // global display state (it switches the desktop resolution before the device is created), and
        // on a hard crash the desktop can be left at the changed mode. Its one advantage over
        // borderless -- render at a non-native resolution AND fill the screen -- was not being used by
        // anyone. The WRAPPER still implements it, and any config that already selects it keeps
        // working and keeps the option visible; this only stops it being newly chosen.
        static readonly string[] DisplayModes = { "Borderless Fullscreen", "Fullscreen", "Windowed" };
        static readonly string[] DisplayModeKeys = { "borderless", "fullscreen", "windowed" };
        const int ModeBorderless = 0, ModeFullscreen = 1, ModeWindowed = 2;

        // Sizes offered to players. This is the window size in Windowed and the display mode to
        // switch to in Fullscreen; Borderless always follows the monitor it launches on, so the
        // list is disabled there.
        //
        // ⚠ THIS LIST IS ENUMERATED FROM THE DISPLAY, NOT HARDCODED, AND THAT IS THE WHOLE POINT.
        // It used to be a fixed list of nine sizes. The client refuses to start on a size the
        // graphics driver does not report as a display mode -- it exits with code 1 about a fifth
        // of a second in, no window, no dialog, no crash dump, which reads to a player as "the game
        // does nothing". That check is meaningless in Windowed mode (a window can be any size) but
        // the client enforces it regardless, so we cannot offer sizes the display does not have.
        //
        // Measured on a 2560x1440 RTX 5090: of the nine sizes the old list offered, FOUR were dead
        // (1366x768, 1600x900, 1600x1200, 1280x960) because that driver does not advertise them.
        // A different monitor kills a different four, which is why a fixed list can never be right.
        // Verified by launching the client across 20 sizes: it starts if and only if the size is in
        // EnumDisplaySettings, 20 out of 20.
        static string[] SupportedResolutions()
        {
            var seen = new SortedSet<(int, int)>();
            var dm = new DEVMODE { dmSize = (ushort)Marshal.SizeOf(typeof(DEVMODE)) };
            for (int i = 0; EnumDisplaySettings(null, i, ref dm); i++)
            {
                // 32bpp only, and nothing below the client's own floor.
                if (dm.dmBitsPerPel != 32) continue;
                if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
                seen.Add(((int)dm.dmPelsWidth, (int)dm.dmPelsHeight));
                dm.dmSize = (ushort)Marshal.SizeOf(typeof(DEVMODE));
            }
            // A machine that reports nothing is not a machine we can guess for: fall back to the
            // desktop size, which is by definition a real mode.
            if (seen.Count == 0)
            {
                var b = Screen.PrimaryScreen.Bounds;
                seen.Add((b.Width, b.Height));
            }
            return seen.Select(r => r.Item1 + " x " + r.Item2).ToArray();
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        struct DEVMODE
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public ushort dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public uint dmFields;
            public int dmPositionX, dmPositionY;
            public uint dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public ushort dmLogPixels;
            public uint dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public uint dmICMMethod, dmICMIntent, dmMediaType, dmDitherType;
            public uint dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }

        [DllImport("user32.dll", CharSet = CharSet.Ansi)]
        static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

        // In-game font, stored as the game's own FONT_JPN registry string. "System" is a Windows
        // alias that always resolves, so it is both the default and the safe fallback; the rest are
        // offered only when actually installed, because naming a missing font gets a player nothing.
        // Always offered and always resolvable: "System" is a GDI alias rather than a family, so
        // it never appears in the enumeration and can never be missing. Last-resort fallback.
        const string AliasFont = "System";

        // What a fresh install gets, best first. Verdana over Tahoma deliberately: it has a larger
        // x-height and wider stems, so it stays crisper at the sizes PSO actually renders, and it
        // covers every character the game's text uses (checked against the client's own text
        // archive -- 110 distinct characters, nothing outside Latin-1).
        //
        // NOT "System". It is an alias, and it resolves to different typefaces on different
        // machines -- Microsoft Sans Serif on Windows, something else again under Proton -- so it
        // is the one choice that guarantees players do not see the same thing.
        static readonly string[] DefaultFontPreference = { "Verdana", "Tahoma", AliasFont };

        static string DefaultFontFor(IList<string> available)
        {
            foreach (var f in DefaultFontPreference)
                if (available.Contains(f, StringComparer.OrdinalIgnoreCase))
                    return f;
            return AliasFont;
        }

        // Offered first, when installed: the ones that actually suit this game. The rest of the
        // machine's fonts follow alphabetically, so a player is not limited to our shortlist.
        static readonly string[] PreferredFonts = { "Tahoma", "Verdana", "Arial", "Segoe UI", "MS Gothic", "MS UI Gothic", "Meiryo", "Yu Gothic", "Malgun Gothic", "Dotum", "Gulim" };

        // Fonts whose glyphs are pictures, not letters. Choosing one turns every menu into
        // dingbats, and the way back is the same unreadable menu -- so they are never offered.
        static readonly string[] SymbolFonts = { "Wingdings", "Wingdings 2", "Wingdings 3", "Webdings", "Symbol", "Marlett", "Bookshelf Symbol 7", "MT Extra", "HoloLens MDL2 Assets", "Segoe MDL2 Assets", "Segoe Fluent Icons" };

        /// <summary>Every font on this machine the game could sensibly use, best-known first.</summary>
        static string[] AvailableFonts()
        {
            // "System" is a GDI alias rather than a family, so it never appears in the collection.
            // It always resolves, which is what makes it the safe default.
            var list = new List<string> { AliasFont };
            var installed = new List<string>();
            try
            {
                using (var c = new InstalledFontCollection())
                    foreach (var f in c.Families) installed.Add(f.Name);
            }
            catch
            {
                // No font enumeration means no informed choice; System alone still works.
                return list.ToArray();
            }

            var skip = new HashSet<string>(SymbolFonts, StringComparer.OrdinalIgnoreCase);
            var usable = installed
                .Where(f => !skip.Contains(f))
                .Where(f => !f.StartsWith("Segoe UI Variable", StringComparison.OrdinalIgnoreCase))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();

            foreach (var f in PreferredFonts)
            {
                var hit = usable.FirstOrDefault(u => string.Equals(u, f, StringComparison.OrdinalIgnoreCase));
                if (hit != null && !list.Contains(hit, StringComparer.OrdinalIgnoreCase)) list.Add(hit);
            }
            list.AddRange(usable.Where(u => !list.Contains(u, StringComparer.OrdinalIgnoreCase))
                                .OrderBy(u => u, StringComparer.OrdinalIgnoreCase));
            return list.ToArray();
        }

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
            ClientSize = new Size(430, 604);
            Font = new Font("Segoe UI", 9f);

            var title = new Label {
                Text = "Corellia Options", AutoSize = true, Location = new Point(16, 12),
                Font = new Font("Segoe UI", 15f, FontStyle.Bold)
            };
            Controls.Add(title);

            // Display
            var gDisplay = new GroupBox { Text = "Display", Location = new Point(16, 50), Size = new Size(398, 128) };
            var lblMode = new Label { Text = "Mode:", Location = new Point(14, 28), AutoSize = true };
            cboMode = new ComboBox { Location = new Point(150, 24), Size = new Size(200, 24), DropDownStyle = ComboBoxStyle.DropDownList };
            // Items are filled in by BuildModeList once the config has been read.
            var lblRes = new Label { Text = "Resolution:", Location = new Point(14, 60), AutoSize = true };
            cboRes = new ComboBox { Location = new Point(150, 56), Size = new Size(200, 24), DropDownStyle = ComboBoxStyle.DropDownList };
            cboRes.Items.AddRange(SupportedResolutions());
            var lblFont = new Label { Text = "Font:", Location = new Point(14, 92), AutoSize = true };
            cboFont = new ComboBox { Location = new Point(150, 88), Size = new Size(200, 24), DropDownStyle = ComboBoxStyle.DropDownList };
            cboFont.Items.AddRange(AvailableFonts());
            foreach (var c in new Control[] { lblMode, cboMode, lblRes, cboRes, lblFont, cboFont }) gDisplay.Controls.Add(c);
            Controls.Add(gDisplay);
            // Borderless always fills the monitor it launches on, so a resolution choice would be a
            // lie there; the other two both honour it.
            cboMode.SelectedIndexChanged += (s, e) => cboRes.Enabled = SelectedMode() != ModeBorderless;

            // Effects
            var gFx = new GroupBox { Text = "Effects", Location = new Point(16, 188), Size = new Size(398, 96) };
            cbSMAA = new CheckBox { Text = "SMAA (anti-alias)", Location = new Point(14, 26), AutoSize = true };
            cbSSAO = new CheckBox { Text = "SSAO", Location = new Point(160, 26), AutoSize = true };
            cbCel  = new CheckBox { Text = "Cel shading", Location = new Point(270, 26), AutoSize = true };
            cbDOF  = new CheckBox { Text = "Depth of field", Location = new Point(14, 58), AutoSize = true };
            cbHDR  = new CheckBox { Text = "HDR", Location = new Point(160, 58), AutoSize = true };
            cbMSAA = new CheckBox { Text = "MSAA", Location = new Point(270, 58), AutoSize = true };
            foreach (var c in new Control[] { cbSMAA, cbSSAO, cbCel, cbDOF, cbHDR, cbMSAA }) gFx.Controls.Add(c);
            Controls.Add(gFx);

            // Sharpening (post-process) — crisps the 3D scene; leaves the HUD/text overlay untouched.
            var gSharp = new GroupBox { Text = "Sharpening (3D scene)", Location = new Point(16, 294), Size = new Size(398, 54) };
            cbSceneSharpen = new CheckBox { Text = "Enabled", Location = new Point(14, 24), AutoSize = true };
            var lblS1 = new Label { Text = "Strength:", Location = new Point(150, 25), AutoSize = true };
            cboSceneSharpen = new ComboBox { Location = new Point(218, 21), Size = new Size(70, 24), DropDownStyle = ComboBoxStyle.DropDownList };
            cboSceneSharpen.Items.AddRange(SharpenStrengths);
            foreach (var c in new Control[] { cbSceneSharpen, lblS1, cboSceneSharpen }) gSharp.Controls.Add(c);
            cbSceneSharpen.CheckedChanged += (s, e) => cboSceneSharpen.Enabled = cbSceneSharpen.Checked;
            Controls.Add(gSharp);

            // Sound. The game itself has no volume control of any kind -- not in game, and the setup
            // tool's sound page is switches only -- so these three are the only ones players get.
            // They are read by our proxy dsound.dll, which scales every sound buffer the game
            // creates. Music and effects are genuinely separable: the streaming buffers the music
            // engine uses are distinguishable from one-shot effects, measured over 6,363 buffers.
            // Values are percentages; the proxy converts to decibels, which is what DirectSound
            // actually wants.
            var gSound = new GroupBox { Text = "Sound", Location = new Point(16, 354), Size = new Size(398, 128) };
            tbMaster = AddVolumeSlider(gSound, "Master:", 24);
            tbMusic = AddVolumeSlider(gSound, "Music:", 56);
            tbEffects = AddVolumeSlider(gSound, "Effects:", 88);
            Controls.Add(gSound);

            // Controller button prompts (HD UI Controller Edition): swaps f256_hyouji.prs so on-screen
            // button hints suit a gamepad (e.g. Palette Swap shows "R" instead of "Ctrl").
            cbController = new CheckBox { Text = "Controller button prompts", Location = new Point(24, 490), AutoSize = true };
            Controls.Add(cbController);

            // Remember login — toggles the game's own ACCOUNT_CHECK / PASSWORD_CHECK registry flags
            // (like the native option). We never read or write the credentials themselves.
            cbSaveLogin = new CheckBox { Text = "Save ID and Password", Location = new Point(24, 514), AutoSize = true };
            Controls.Add(cbSaveLogin);

            var btnSave = new Button { Text = "Save && Close", Location = new Point(150, 544), Size = new Size(130, 44) };
            btnSave.Font = new Font("Segoe UI", 11f, FontStyle.Bold);
            btnSave.Click += OnSaveClose;
            Controls.Add(btnSave);
            AcceptButton = btnSave;
        }

        // One row of the Sound group: label, slider, live percentage readout. TrackBar has no
        // continuous scale, so the slider works in steps of 5 -- fine for volume, and it keeps the
        // tick marks legible.
        static TrackBar AddVolumeSlider(GroupBox parent, string caption, int y)
        {
            var label = new Label { Text = caption, Location = new Point(14, y + 7), AutoSize = true };
            var bar = new TrackBar {
                // AutoSize defaults to TRUE on TrackBar, which makes it IGNORE Size and claim about
                // 45px of height for its tick gutter. Three of those at 28px spacing overlapped each
                // other, leaked stray tick marks into the neighbouring rows, and pushed the last
                // slider through the bottom of the group box. Turn it off and take the height back.
                AutoSize = false,
                TickStyle = TickStyle.None,
                Location = new Point(78, y),
                Size = new Size(250, 30),
                Minimum = 0,
                Maximum = 100,
                SmallChange = 5,
                LargeChange = 20,
                Value = 100,
            };
            var pct = new Label { Text = "100%", Location = new Point(338, y + 7), AutoSize = true };
            bar.ValueChanged += (s, e) => {
                bar.Value = (bar.Value / 5) * 5;
                pct.Text = bar.Value + "%";
            };
            parent.Controls.Add(label);
            parent.Controls.Add(bar);
            parent.Controls.Add(pct);
            return bar;
        }

        static void SetVolume(TrackBar bar, Dictionary<string, string> d, string key)
        {
            int v = 100;
            if (d.TryGetValue(key, out var s) && int.TryParse(s.Trim(), out var i))
                v = i < 0 ? 0 : (i > 100 ? 100 : i);
            bar.Value = (v / 5) * 5;
        }

        // Fill the dropdown, offering Fullscreen only when the config already selects it, so an
        // existing player keeps their setting and can see what it is. Returns the position to select.
        int BuildModeList(int mode)
        {
            offeredModes.Clear();
            offeredModes.Add(ModeBorderless);
            if (mode == ModeFullscreen) offeredModes.Add(ModeFullscreen);
            offeredModes.Add(ModeWindowed);

            cboMode.Items.Clear();
            foreach (var m in offeredModes) cboMode.Items.Add(DisplayModes[m]);

            int pos = offeredModes.IndexOf(mode);
            return pos < 0 ? offeredModes.IndexOf(ModeBorderless) : pos;
        }

        // The dropdown position is not the mode index once Fullscreen is hidden; always go through here.
        int SelectedMode()
        {
            int i = cboMode.SelectedIndex;
            if (i < 0 || i >= offeredModes.Count) return ModeBorderless;
            return offeredModes[i];
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

            // Default to the desktop size rather than a fixed one: it is the only size guaranteed
            // to be a real display mode on this machine.
            var desktop = Screen.PrimaryScreen.Bounds;
            int w = ParseInt(d, "WindowWidth", desktop.Width);
            int h = ParseInt(d, "WindowHeight", desktop.Height);
            string res = w + " x " + h;
            int idx = cboRes.Items.IndexOf(res);
            if (idx < 0)
            {
                // The saved size is not a mode this display has, so the client would refuse to
                // start on it. Do NOT add it back to the list -- that is how a player stays stuck
                // on a config that silently does nothing. Fall back to the desktop size, which
                // always works, and let Save write the correction out.
                idx = cboRes.Items.IndexOf(desktop.Width + " x " + desktop.Height);
                if (idx < 0) idx = cboRes.Items.Count - 1;
            }
            if (idx >= 0) cboRes.SelectedIndex = idx;

            // Font lives in the game's own registry, not widescreen.cfg.
            string font = ReadFont();
            int fidx = cboFont.Items.IndexOf(font);
            if (fidx < 0) fidx = cboFont.Items.IndexOf(DefaultFontFor(cboFont.Items.Cast<string>().ToList()));
            if (fidx >= 0) cboFont.SelectedIndex = fidx;

            // DisplayMode is the current key; Windowed= is what pre-2026-08 configs carry and what
            // an older d3d8.dll still reads, so it is honoured on the way in and rewritten on the
            // way out. DisplayMode wins when both are present -- same precedence as the wrapper.
            int mode = ModeBorderless;
            if (d.TryGetValue("DisplayMode", out var dm))
            {
                int i = Array.FindIndex(DisplayModeKeys, k => k.Equals(dm.Trim(), StringComparison.OrdinalIgnoreCase));
                if (i >= 0) mode = i;
            }
            else if (AsBool(d, "Windowed", false))
            {
                mode = ModeWindowed;
            }
            cboMode.SelectedIndex = BuildModeList(mode);
            cboRes.Enabled = mode != ModeBorderless;

            // Absent keys mean 100%, i.e. exactly how the game sounded before this feature existed.
            SetVolume(tbMaster, d, "MasterVolume");
            SetVolume(tbMusic, d, "MusicVolume");
            SetVolume(tbEffects, d, "EffectVolume");
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
            SetKey(lines, "MasterVolume", tbMaster.Value.ToString());
            SetKey(lines, "MusicVolume", tbMusic.Value.ToString());
            SetKey(lines, "EffectVolume", tbEffects.Value.ToString());
            // HUD scaling is entangled with the widescreen layout math in the wrapper (non-1.0
            // leaves a seam), so it's not exposed — lock it to the value that renders correctly.
            SetKey(lines, "HUDScale", "1.0");

            // INVARIANT: the display mode is a widescreen.cfg KEY the wrapper reads. It must never go
            // back to the old approach of renaming d3d8.dll to d3d8.dll.off — that DLL is also the
            // ASI loader, so disabling it silently drops patches/largeassets.asi, the engine then
            // reads a bundled HD .bml into a vanilla-sized buffer, and the client access-violates on
            // the first over-cap area (reliably Caves 1). The wrapper stays loaded in every mode.
            int mode = SelectedMode();
            SetKey(lines, "DisplayMode", DisplayModeKeys[mode]);
            // Written for compatibility both ways: an older d3d8.dll ignores DisplayMode entirely and
            // would otherwise keep whatever Windowed= said last. Fullscreen has no old-wrapper
            // equivalent, so it degrades to borderless there rather than to a small window.
            SetKey(lines, "Windowed", mode == ModeWindowed ? "1" : "0");
            ParseRes((string)cboRes.SelectedItem, out int w, out int h);
            SetKey(lines, "WindowWidth", w.ToString());
            SetKey(lines, "WindowHeight", h.ToString());

            WriteFont(cboFont.SelectedItem as string);

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

        // ---- In-game font (the game's own FONT_JPN value) ----------------------
        //
        // ⚠ WRITTEN TO THIS BUILD'S OWN REGISTRY LEAF, never to the shared SonicTeam\PSOBB key.
        // The client tree still ships font_tahoma.reg / font_verdana.reg / font_dotum.reg, and all
        // three of those target PSOBB -- which since the per-build registry split is EPHINEA's key,
        // not ours. Running one changes Ephinea's font and does nothing for Corellia. They should
        // be dropped from the payload now that this dropdown exists; do not copy their target.
        static string ReadFont()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(PsoRegPath))
                {
                    var v = k?.GetValue("FONT_JPN") as string;
                    if (!string.IsNullOrWhiteSpace(v)) return v.Trim();
                }
            }
            catch { }
            return "";   // unset: the caller picks the default from what is installed
        }

        static void WriteFont(string font)
        {
            if (string.IsNullOrWhiteSpace(font)) font = DefaultFontFor(AvailableFonts());
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(PsoRegPath))
                    k?.SetValue("FONT_JPN", font, RegistryValueKind.String);
            }
            catch { }
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
