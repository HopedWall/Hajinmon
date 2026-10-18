using System.Diagnostics;
using System.Text;

namespace Hajinmon.Launcher;

/// <summary>Main launcher window: settings tabs plus the Play button.</summary>
public sealed class MainForm : Form
{
    private static readonly (string Label, int Width, int Height)[] WindowSizes =
    {
        ("1280 x 720", 1280, 720),
        ("1600 x 900", 1600, 900),
        ("1920 x 1080", 1920, 1080),
        ("2560 x 1440", 2560, 1440),
        ("3840 x 2160", 3840, 2160),
    };

    private static readonly (string Label, int Value)[] ScaleOptions =
    {
        ("1x (native 720p)", 1),
        ("2x (1440p)", 2),
        ("3x (2160p)", 3),
        ("4x (2880p)", 4),
    };

    private static readonly (string Label, int Value)[] AnisotropicOptions =
    {
        ("Game default", -1),
        ("Off", 0),
        ("2x", 2),
        ("4x", 3),
        ("8x", 4),
        ("16x", 5),
    };

    private static readonly (string Label, string Value)[] AntiAliasingOptions =
    {
        ("Off", "none"),
        ("FXAA", "fxaa"),
        ("FXAA (extreme)", "fxaa_extreme"),
    };

    private static readonly (string Label, string Value)[] ScalingOptions =
    {
        ("Bilinear", "bilinear"),
        ("AMD FidelityFX CAS (sharpen)", "cas"),
        ("AMD FidelityFX FSR (upscale, use with resolution scale 1x)", "fsr"),
    };

    private static readonly (string Label, string Value)[] ApiOptions =
    {
        ("Direct3D 12 (recommended)", "d3d12"),
        ("Vulkan (experimental)", "vulkan"),
    };

    private static readonly (string Label, int Value)[] LanguageOptions =
    {
        ("English", 1),
        ("Japanese", 2),
        ("German", 3),
        ("French", 4),
        ("Spanish", 5),
        ("Italian", 6),
    };

    private readonly Settings _settings;

    // Graphics
    private readonly ComboBox _displayMode = Combo();
    private readonly ComboBox _windowSize = Combo();
    private readonly ComboBox _resolutionScale = Combo();
    private readonly CheckBox _vsync = new() { Text = "VSync (recommended: the game runs at 60 FPS)", AutoSize = true };
    private readonly ComboBox _anisotropic = Combo();
    private readonly ComboBox _antiAliasing = Combo();
    private readonly ComboBox _scaling = Combo();
    private readonly CheckBox _keepAspect = new() { Text = "Keep 16:9 aspect ratio (black bars on other ratios)", AutoSize = true };
    private readonly ComboBox _api = Combo();
    private readonly ComboBox _gpu = Combo();
    // The GPU list depends on the API: DXGI and Vulkan number the GPUs differently.
    private readonly List<int> _gpuIndices = new();
    private List<GpuList.Adapter>? _dxgiAdapters;
    private List<VulkanGpuList.Device>? _vulkanDevices;
    private string? _gpuListApi;
    private Settings? _gpuSource;

    // Game
    private readonly ComboBox _language = Combo();
    private readonly TextBox _saveFolder = new() { Width = 280 };
    private readonly TextBox _gameFolder = new() { Width = 280 };
    private readonly TextBox _gameExe = new() { Width = 280 };

    // Controls
    private readonly ControlsPage _controls = new();

    private bool _loading;

    // Cheats
    private readonly CheckBox _unlockCharacters = new() { Text = "Unlock all characters", AutoSize = true };
    private readonly CheckBox _unlockCostumes = new() { Text = "Unlock all costumes", AutoSize = true };
    private readonly CheckBox _backupSave = new() { Text = "Back up the save data before launching with cheats", AutoSize = true };

    // Advanced
    private readonly TextBox _extraArgs = new() { Width = 460 };
    private readonly TextBox _commandPreview = new()
    {
        Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical,
        Width = 460, Height = 200, Font = new Font(FontFamily.GenericMonospace, 8.5f),
    };

    public MainForm()
    {
        _settings = Settings.Load();

        Text = "Hajinmon - Dead or Alive 4 Launcher";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(560, 540);
        Font = new Font("Segoe UI", 9f);

        var tabs = new TabControl { Location = new Point(10, 10), Size = new Size(540, 475) };
        tabs.TabPages.Add(BuildGraphicsTab());
        tabs.TabPages.Add(BuildGameTab());
        tabs.TabPages.Add(_controls);
        tabs.TabPages.Add(BuildCheatsTab());
        tabs.TabPages.Add(BuildAdvancedTab());
        tabs.SelectedIndexChanged += (_, _) => UpdatePreview();
        _controls.Changed += (_, _) => UpdatePreview();
        Controls.Add(tabs);

        var play = new Button { Text = "Play", Size = new Size(110, 32), Location = new Point(440, 498) };
        play.Font = new Font(Font, FontStyle.Bold);
        play.Click += (_, _) => Launch();
        var save = new Button { Text = "Save settings", Size = new Size(110, 32), Location = new Point(320, 498) };
        save.Click += (_, _) => { ReadControls(); TrySave(); };
        var reset = new Button { Text = "Defaults", Size = new Size(90, 32), Location = new Point(10, 498) };
        reset.Click += (_, _) => ResetDefaults();
        Controls.AddRange(new Control[] { play, save, reset });
        AcceptButton = play;

        LoadControls(_settings);
        FormClosing += (_, _) => { ReadControls(); TrySave(); };
        Shown += (_, _) => AskForGameIfMissing();
    }

    // ---------------------------------------------------------------- layout

    private static ComboBox Combo() => new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 290 };

    private static TableLayoutPanel Grid()
    {
        var grid = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(10), ColumnCount = 2, AutoScroll = true,
        };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        return grid;
    }

    private static void AddRow(TableLayoutPanel grid, string? label, Control control)
    {
        int row = grid.RowCount++;
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        if (label != null)
        {
            grid.Controls.Add(new Label
            {
                Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 8, 10, 3),
            }, 0, row);
            control.Margin = new Padding(3, 4, 3, 4);
            grid.Controls.Add(control, 1, row);
        }
        else
        {
            control.Margin = new Padding(3, 6, 3, 2);
            grid.Controls.Add(control, 0, row);
            grid.SetColumnSpan(control, 2);
        }
    }

    private static Label Note(string text) => new()
    {
        Text = text, AutoSize = true, MaximumSize = new Size(440, 0), ForeColor = SystemColors.GrayText,
    };

    private static Control PathPicker(TextBox box, bool folder, string filter = "")
    {
        var panel = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
        box.Margin = new Padding(0, 2, 4, 0);
        var browse = new Button { Text = "...", Width = 32, Height = box.Height + 2, Margin = Padding.Empty };
        browse.Click += (_, _) =>
        {
            if (folder)
            {
                using var dialog = new FolderBrowserDialog { SelectedPath = box.Text, UseDescriptionForTitle = true };
                if (dialog.ShowDialog() == DialogResult.OK) box.Text = dialog.SelectedPath;
            }
            else
            {
                using var dialog = new OpenFileDialog { Filter = filter, FileName = box.Text };
                if (dialog.ShowDialog() == DialogResult.OK) box.Text = dialog.FileName;
            }
        };
        panel.Controls.Add(box);
        panel.Controls.Add(browse);
        return panel;
    }

    /// <summary>Path box whose browse button picks either a folder or a disc image.</summary>
    private static Control GamePicker(TextBox box)
    {
        var panel = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
        box.Margin = new Padding(0, 2, 4, 0);
        var browse = new Button { Text = "...", Width = 32, Height = box.Height + 2, Margin = Padding.Empty };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Disc image (.iso)...", null, (_, _) => BrowseIso(box));
        menu.Items.Add("Folder with the extracted files...", null, (_, _) =>
        {
            using var dialog = new FolderBrowserDialog
            {
                SelectedPath = Directory.Exists(box.Text) ? box.Text : "", UseDescriptionForTitle = true,
                Description = "Folder extracted from the ISO (it contains default.xex)",
            };
            if (dialog.ShowDialog() == DialogResult.OK) box.Text = dialog.SelectedPath;
        });
        browse.Click += (_, _) => menu.Show(browse, new Point(0, browse.Height));
        panel.Controls.Add(box);
        panel.Controls.Add(browse);
        return panel;
    }

    private static bool BrowseIso(TextBox box)
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Select your Dead or Alive 4 disc image",
            Filter = "Xbox 360 disc image (*.iso)|*.iso|All files|*.*",
            FileName = File.Exists(box.Text) ? box.Text : "",
        };
        if (dialog.ShowDialog() != DialogResult.OK) return false;
        box.Text = dialog.FileName;
        return true;
    }

    /// <summary>First start without a game: ask for the player's own copy.</summary>
    private void AskForGameIfMissing()
    {
        if (IsGamePath(EffectiveGameFolder())) return;
        var answer = MessageBox.Show(this,
            "Hajinmon needs your own copy of Dead or Alive 4 for Xbox 360 (a disc image, .iso).\n\n" +
            "Select it now? You can change it later in the Game tab.",
            Text, MessageBoxButtons.YesNo, MessageBoxIcon.Information);
        if (answer == DialogResult.Yes && BrowseIso(_gameFolder))
        {
            ReadControls();
            TrySave();
        }
    }

    private TabPage BuildGraphicsTab()
    {
        var page = new TabPage("Graphics");
        var grid = Grid();

        _displayMode.Items.AddRange(new object[] { "Windowed", "Fullscreen" });
        foreach (var size in WindowSizes) _windowSize.Items.Add(size.Label);
        foreach (var option in ScaleOptions) _resolutionScale.Items.Add(option.Label);
        foreach (var option in AnisotropicOptions) _anisotropic.Items.Add(option.Label);
        foreach (var option in AntiAliasingOptions) _antiAliasing.Items.Add(option.Label);
        foreach (var option in ScalingOptions) _scaling.Items.Add(option.Label);

        foreach (var option in ApiOptions) _api.Items.Add(option.Label);
        _api.SelectedIndexChanged += (_, _) => OnApiChanged();
        _displayMode.SelectedIndexChanged += (_, _) => _windowSize.Enabled = _displayMode.SelectedIndex == 0;

        AddRow(grid, "Graphics API", _api);
        AddRow(grid, "Display mode", _displayMode);
        AddRow(grid, "Window size", _windowSize);
        AddRow(grid, "Internal resolution", _resolutionScale);
        AddRow(grid, "Texture filtering", _anisotropic);
        AddRow(grid, "Anti-aliasing", _antiAliasing);
        AddRow(grid, "Scaling filter", _scaling);
        AddRow(grid, "Graphics card", _gpu);
        AddRow(grid, null, _vsync);
        AddRow(grid, null, _keepAspect);
        AddRow(grid, null, Note(
            "The internal resolution is the size the game renders at (the Xbox 360 renders at 720p). " +
            "Higher values look sharper but need a faster GPU. FSR works best when the window is " +
            "larger than the internal resolution."));

        page.Controls.Add(grid);
        return page;
    }

    private string SelectedApi => ApiOptions[Math.Max(0, _api.SelectedIndex)].Value;

    /// <summary>Keeps the GPU list in step with the selected graphics API.</summary>
    private void OnApiChanged()
    {
        // Remember the GPU picked for the previous API before switching lists.
        if (_gpuListApi != null && !_loading) StoreGpuSelection(_settings, _gpuListApi);
        var source = _gpuSource ?? _settings;
        string api = SelectedApi;
        int selected = api == "vulkan" ? source.VulkanDevice : source.GpuAdapter;

        _gpu.Items.Clear();
        _gpuIndices.Clear();
        if (api == "vulkan")
        {
            _vulkanDevices ??= VulkanGpuList.Enumerate();
            _gpu.Items.Add("Automatic (dedicated GPU)");
            _gpuIndices.Add(-1);
            foreach (var device in _vulkanDevices)
            {
                _gpu.Items.Add(device.Dedicated ? $"{device.Name} (dedicated)" : $"{device.Name} (integrated)");
                _gpuIndices.Add(device.Index);
            }
        }
        else
        {
            _dxgiAdapters ??= GpuList.Enumerate();
            _gpu.Items.Add("Automatic (high-performance GPU)");
            _gpuIndices.Add(-1);
            foreach (var adapter in _dxgiAdapters)
            {
                _gpu.Items.Add($"{adapter.Name} ({adapter.VideoMemory / (1024 * 1024)} MB)");
                _gpuIndices.Add(adapter.Index);
            }
        }
        int index = _gpuIndices.IndexOf(selected);
        _gpu.SelectedIndex = index >= 0 ? index : 0;
        _gpuListApi = api;
        UpdatePreview();
    }

    private void StoreGpuSelection(Settings s, string api)
    {
        int index = _gpu.SelectedIndex > 0 ? _gpuIndices[_gpu.SelectedIndex] : -1;
        if (api == "vulkan") s.VulkanDevice = index;
        else s.GpuAdapter = index;
    }

    private TabPage BuildGameTab()
    {
        var page = new TabPage("Game");
        var grid = Grid();

        foreach (var option in LanguageOptions) _language.Items.Add(option.Label);
        _saveFolder.PlaceholderText = DefaultSaveFolder();
        _gameFolder.PlaceholderText = DefaultGameFolder() ?? "Disc image (.iso) or extracted folder";
        _gameExe.PlaceholderText = DefaultGameExecutable() ?? "HajinmonGame.exe";

        AddRow(grid, "Language", _language);
        AddRow(grid, "Save folder", PathPicker(_saveFolder, folder: true));
        var openSaves = new LinkLabel { Text = "Open save folder", AutoSize = true };
        openSaves.LinkClicked += (_, _) => OpenFolder(EffectiveSaveFolder());
        AddRow(grid, "", openSaves);
        AddRow(grid, "Game (ISO or folder)", GamePicker(_gameFolder));
        AddRow(grid, "Game executable", PathPicker(_gameExe, folder: false, "HajinmonGame.exe|HajinmonGame.exe|Executables|*.exe"));
        AddRow(grid, null, Note(
            "Leave a path empty to use the default shown in grey. The save folder holds the " +
            "profile, the game save and the shader cache; it is created on first launch. " +
            "The game can be the disc image (.iso) itself or the folder extracted from it " +
            "(it contains default.xex)."));

        page.Controls.Add(grid);
        return page;
    }

    private TabPage BuildCheatsTab()
    {
        var page = new TabPage("Cheats");
        var grid = Grid();

        AddRow(grid, null, _unlockCharacters);
        AddRow(grid, null, Note("Makes every character selectable, including the hidden ones."));
        AddRow(grid, null, _unlockCostumes);
        AddRow(grid, null, Note("Unlocks every costume of every character."));
        AddRow(grid, null, _backupSave);
        AddRow(grid, null, Note(
            "Cheats are applied once the profile has been loaded (after PRESS START). The game " +
            "writes the unlocks into the save the next time it saves, so they become permanent: " +
            "turning the cheat off later does not lock them again. The backup (a copy of the save " +
            "folder's content directory) lets you go back."));
        var openBackups = new LinkLabel { Text = "Open backup folder", AutoSize = true };
        openBackups.LinkClicked += (_, _) => OpenFolder(BackupRoot());
        AddRow(grid, null, openBackups);

        page.Controls.Add(grid);
        return page;
    }

    private TabPage BuildAdvancedTab()
    {
        var page = new TabPage("Advanced");
        var grid = Grid();

        _extraArgs.PlaceholderText = "e.g. --log_guest_fps";
        _extraArgs.TextChanged += (_, _) => UpdatePreview();
        AddRow(grid, null, new Label { Text = "Extra command line options:", AutoSize = true });
        AddRow(grid, null, _extraArgs);
        AddRow(grid, null, new Label { Text = "Command line that will be used:", AutoSize = true });
        AddRow(grid, null, _commandPreview);

        page.Controls.Add(grid);
        return page;
    }

    // ------------------------------------------------------- settings <-> UI

    private static int IndexOf<T>((string Label, T Value)[] options, T value, int fallback)
    {
        for (int i = 0; i < options.Length; i++)
        {
            if (EqualityComparer<T>.Default.Equals(options[i].Value, value)) return i;
        }
        return fallback;
    }

    private void LoadControls(Settings s)
    {
        // Change events fire while the controls are filled: don't read them back
        // into the settings half-loaded.
        _loading = true;
        try
        {
            FillControls(s);
        }
        finally
        {
            _loading = false;
        }
        UpdatePreview();
    }

    private void FillControls(Settings s)
    {
        _displayMode.SelectedIndex = s.Fullscreen ? 1 : 0;
        int size = Array.FindIndex(WindowSizes, w => w.Width == s.WindowWidth && w.Height == s.WindowHeight);
        _windowSize.SelectedIndex = size >= 0 ? size : 2;
        _windowSize.Enabled = !s.Fullscreen;
        _resolutionScale.SelectedIndex = IndexOf(ScaleOptions, s.ResolutionScale, 1);
        _vsync.Checked = s.VSync;
        _anisotropic.SelectedIndex = IndexOf(AnisotropicOptions, s.Anisotropic, 0);
        _antiAliasing.SelectedIndex = IndexOf(AntiAliasingOptions, s.AntiAliasing, 0);
        _scaling.SelectedIndex = IndexOf(ScalingOptions, s.ScalingFilter, 0);
        _keepAspect.Checked = s.KeepAspectRatio;
        // Fills the GPU list for this API (with the GPU stored in s).
        _gpuSource = s;
        _gpuListApi = null;
        int api = Array.FindIndex(ApiOptions, o => o.Value == s.GraphicsApi);
        _api.SelectedIndex = -1;
        _api.SelectedIndex = api >= 0 ? api : 0;
        _gpuSource = null;

        _language.SelectedIndex = IndexOf(LanguageOptions, s.Language, 0);
        _saveFolder.Text = s.SaveFolder;
        _gameFolder.Text = s.GameFolder;
        _gameExe.Text = s.GameExecutable;
        _controls.LoadFrom(s);

        _unlockCharacters.Checked = s.UnlockAllCharacters;
        _unlockCostumes.Checked = s.UnlockAllCostumes;
        _backupSave.Checked = s.BackupSaveBeforeCheats;

        _extraArgs.Text = s.ExtraArguments;
    }

    private void ReadControls()
    {
        var s = _settings;
        s.Fullscreen = _displayMode.SelectedIndex == 1;
        s.WindowWidth = WindowSizes[_windowSize.SelectedIndex].Width;
        s.WindowHeight = WindowSizes[_windowSize.SelectedIndex].Height;
        s.ResolutionScale = ScaleOptions[_resolutionScale.SelectedIndex].Value;
        s.VSync = _vsync.Checked;
        s.Anisotropic = AnisotropicOptions[_anisotropic.SelectedIndex].Value;
        s.AntiAliasing = AntiAliasingOptions[_antiAliasing.SelectedIndex].Value;
        s.ScalingFilter = ScalingOptions[_scaling.SelectedIndex].Value;
        s.KeepAspectRatio = _keepAspect.Checked;
        s.GraphicsApi = SelectedApi;
        if (_gpuListApi != null) StoreGpuSelection(s, _gpuListApi);

        s.Language = LanguageOptions[_language.SelectedIndex].Value;
        s.SaveFolder = _saveFolder.Text.Trim();
        s.GameFolder = _gameFolder.Text.Trim();
        s.GameExecutable = _gameExe.Text.Trim();
        _controls.SaveTo(s);

        s.UnlockAllCharacters = _unlockCharacters.Checked;
        s.UnlockAllCostumes = _unlockCostumes.Checked;
        s.BackupSaveBeforeCheats = _backupSave.Checked;

        s.ExtraArguments = _extraArgs.Text.Trim();
    }

    private void ResetDefaults()
    {
        ReadControls();
        var s = _settings;
        var defaults = new Settings
        {
            // Keep the paths and the controls (they have their own reset).
            SaveFolder = s.SaveFolder, GameFolder = s.GameFolder, GameExecutable = s.GameExecutable,
            KeyboardControls = s.KeyboardControls, MouseLook = s.MouseLook,
            MouseSensitivity = s.MouseSensitivity,
            PadBinds = new(s.PadBinds), KeyBinds = new(s.KeyBinds),
        };
        LoadControls(defaults);
    }

    private void TrySave()
    {
        try
        {
            _settings.Save();
        }
        catch (Exception e)
        {
            MessageBox.Show(this, $"Could not save the settings:\n{e.Message}", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void UpdatePreview()
    {
        if (_loading || _windowSize.SelectedIndex < 0) return;
        ReadControls();
        string exe = EffectiveGameExecutable() ?? "HajinmonGame.exe";
        _commandPreview.Text = $"\"{exe}\" {BuildArguments()}";
    }

    // ------------------------------------------------------------- defaults

    /// <summary>
    /// Where the game stores user data when no folder is given. Older builds used
    /// Documents\doa4; the game moves it to Documents\Hajinmon on its next start,
    /// so until then the old folder is the one holding the saves.
    /// </summary>
    private static string DefaultSaveFolder()
    {
        string documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        string current = Path.Combine(documents, "Hajinmon");
        string legacy = Path.Combine(documents, "doa4");
        return !Directory.Exists(current) && Directory.Exists(legacy) ? legacy : current;
    }

    /// <summary>
    /// Looks for <paramref name="relative"/> next to the launcher and in its parent
    /// folders, which covers both a packaged release and a source checkout.
    /// </summary>
    private static string? FindUpwards(params string[] relative)
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int depth = 0; dir != null && depth < 8; depth++, dir = dir.Parent)
        {
            foreach (string candidate in relative)
            {
                string path = Path.Combine(dir.FullName, candidate);
                if (File.Exists(path) || Directory.Exists(path)) return path;
            }
        }
        return null;
    }

    /// <summary>The runtime built into release launchers, or a local build of the project.</summary>
    private static string? DefaultGameExecutable() =>
        GameRuntime.IsEmbedded ? GameRuntime.ExecutablePath : FindUpwards("HajinmonGame.exe", Path.Combine("doa4", "out", "build", "win-amd64-relwithdebinfo", "HajinmonGame.exe"),
                    Path.Combine("doa4", "out", "build", "win-amd64-release", "HajinmonGame.exe"));

    /// <summary>
    /// The extracted "game" folder if there is one, otherwise the first .iso found
    /// next to the launcher or in the folders above it.
    /// </summary>
    private static string? DefaultGameFolder()
    {
        string? xex = FindUpwards(Path.Combine("game", "default.xex"));
        if (xex != null) return Path.GetDirectoryName(xex);
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int depth = 0; dir != null && depth < 8; depth++, dir = dir.Parent)
        {
            try
            {
                var iso = dir.EnumerateFiles("*.iso").FirstOrDefault();
                if (iso != null) return iso.FullName;
            }
            catch (Exception)
            {
                // Unreadable folder: keep looking above it.
            }
        }
        return null;
    }

    /// <summary>A disc image file, or a folder containing default.xex.</summary>
    private static bool IsGamePath(string? path) =>
        path != null && (File.Exists(path) || File.Exists(Path.Combine(path, "default.xex")));

    private string EffectiveSaveFolder() =>
        string.IsNullOrWhiteSpace(_settings.SaveFolder) ? DefaultSaveFolder() : _settings.SaveFolder;

    private string? EffectiveGameFolder() =>
        string.IsNullOrWhiteSpace(_settings.GameFolder) ? DefaultGameFolder() : _settings.GameFolder;

    private string? EffectiveGameExecutable() =>
        string.IsNullOrWhiteSpace(_settings.GameExecutable) ? DefaultGameExecutable() : _settings.GameExecutable;

    private static string BackupRoot() => Path.Combine(AppPaths.DataFolder, "save_backups");

    // --------------------------------------------------------------- launch

    private static string Quote(string value) => $"\"{value.TrimEnd('\\')}\"";

    private static string Flag(string name, bool value) => value ? $"--{name}" : $"--no-{name}";

    private string BuildArguments()
    {
        var s = _settings;
        var args = new List<string> { "--gpu_plugin=xenos", "--alloc_top_down" };

        string? gameFolder = EffectiveGameFolder();
        if (gameFolder != null) args.Add($"--game_data_root={Quote(gameFolder)}");
        if (!string.IsNullOrWhiteSpace(s.SaveFolder)) args.Add($"--user_data_root={Quote(s.SaveFolder)}");

        args.Add(Flag("fullscreen", s.Fullscreen));
        args.Add($"--window_width={s.WindowWidth}");
        args.Add($"--window_height={s.WindowHeight}");
        args.Add($"--resolution_scale={s.ResolutionScale}");
        args.Add(Flag("vsync", s.VSync));
        // "vsync" only paces the game; these keep the host presentation from tearing too.
        // Without VSync both renderers present as soon as possible (tearing, VRR).
        args.Add(Flag("d3d12_allow_variable_refresh_rate_and_tearing", !s.VSync));
        args.Add(Flag("vulkan_allow_present_mode_immediate", !s.VSync));
        args.Add(Flag("vulkan_allow_present_mode_fifo_relaxed", !s.VSync));
        args.Add($"--anisotropic_override={s.Anisotropic}");
        args.Add($"--swap_post_effect={s.AntiAliasing}");
        args.Add($"--present_effect={s.ScalingFilter}");
        args.Add(Flag("present_letterbox", s.KeepAspectRatio));
        args.Add($"--gpu_backend={s.GraphicsApi}");
        if (s.GraphicsApi == "vulkan")
        {
            if (s.VulkanDevice >= 0) args.Add($"--vulkan_device={s.VulkanDevice}");
        }
        else if (s.GpuAdapter >= 0)
        {
            args.Add($"--d3d12_adapter={s.GpuAdapter}");
        }

        args.Add($"--user_language={s.Language}");
        args.Add(Flag("mnk_mode", s.KeyboardControls));
        args.AddRange(ControlsPage.Arguments(s));

        if (s.UnlockAllCharacters) args.Add("--cheat_unlock_characters");
        if (s.UnlockAllCostumes) args.Add("--cheat_unlock_costumes");

        if (!string.IsNullOrWhiteSpace(s.ExtraArguments)) args.Add(s.ExtraArguments);
        return string.Join(' ', args);
    }

    private void Launch()
    {
        ReadControls();
        TrySave();

        string? exe = EffectiveGameExecutable();
        if (string.IsNullOrWhiteSpace(_settings.GameExecutable) && GameRuntime.IsEmbedded)
        {
            try
            {
                UseWaitCursor = true;
                exe = GameRuntime.Extract();
            }
            catch (Exception e)
            {
                MessageBox.Show(this, $"Could not unpack the game runtime:\n{e.Message}", Text,
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            finally
            {
                UseWaitCursor = false;
            }
        }
        if (exe == null || !File.Exists(exe))
        {
            MessageBox.Show(this, "The game executable (HajinmonGame.exe) was not found. Set \"Game executable\" in the Game tab.",
                            Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        string? gameFolder = EffectiveGameFolder();
        if (!IsGamePath(gameFolder))
        {
            MessageBox.Show(this, "The game was not found. In the Game tab, set \"Game\" to the " +
                                  "disc image (.iso) or to the folder extracted from it (with default.xex).",
                            Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        if (Process.GetProcessesByName("HajinmonGame").Length > 0)
        {
            MessageBox.Show(this, "The game is already running.", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        if ((_settings.UnlockAllCharacters || _settings.UnlockAllCostumes) && _settings.BackupSaveBeforeCheats)
        {
            try
            {
                BackupSave();
            }
            catch (Exception e)
            {
                var answer = MessageBox.Show(this, $"The save backup failed:\n{e.Message}\n\nLaunch anyway?",
                                             Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (answer != DialogResult.Yes) return;
            }
        }

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = exe,
                Arguments = BuildArguments(),
                WorkingDirectory = Path.GetDirectoryName(exe)!,
                UseShellExecute = false,
            });
            Close();
        }
        catch (Exception e)
        {
            MessageBox.Show(this, $"Could not start the game:\n{e.Message}", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    /// <summary>
    /// Copies the save data (everything in the save folder except the shader cache)
    /// into a timestamped folder. Does nothing when there is no save yet.
    /// </summary>
    private void BackupSave()
    {
        var source = new DirectoryInfo(EffectiveSaveFolder());
        if (!source.Exists) return;
        string target = Path.Combine(BackupRoot(), DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss"));
        bool copied = false;
        foreach (var entry in source.EnumerateFileSystemInfos())
        {
            if (entry.Name.Equals("cache", StringComparison.OrdinalIgnoreCase)) continue;
            if (entry is DirectoryInfo dir)
            {
                CopyDirectory(dir, new DirectoryInfo(Path.Combine(target, dir.Name)));
            }
            else
            {
                Directory.CreateDirectory(target);
                ((FileInfo)entry).CopyTo(Path.Combine(target, entry.Name), true);
            }
            copied = true;
        }
        if (copied) File.WriteAllText(Path.Combine(target, "restore.txt"), RestoreNote(source.FullName));
    }

    private static string RestoreNote(string saveFolder)
    {
        var text = new StringBuilder();
        text.AppendLine("Save data backup made by the Hajinmon launcher before launching with cheats.");
        text.AppendLine("To restore it, close the game and copy the content of this folder into:");
        text.AppendLine(saveFolder);
        return text.ToString();
    }

    private static void CopyDirectory(DirectoryInfo source, DirectoryInfo target)
    {
        target.Create();
        foreach (var file in source.EnumerateFiles()) file.CopyTo(Path.Combine(target.FullName, file.Name), true);
        foreach (var dir in source.EnumerateDirectories())
        {
            CopyDirectory(dir, new DirectoryInfo(Path.Combine(target.FullName, dir.Name)));
        }
    }

    // ---------------------------------------------------------------- misc

    private void OpenFolder(string path)
    {
        if (!Directory.Exists(path))
        {
            MessageBox.Show(this, $"The folder does not exist yet:\n{path}", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
    }
}
