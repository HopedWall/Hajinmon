using System.Text.Json;

namespace Hajinmon.Launcher;

/// <summary>Launcher settings, stored as JSON next to the launcher.</summary>
public sealed class Settings
{
    // Graphics
    public bool Fullscreen { get; set; } = false;
    public int WindowWidth { get; set; } = 1920;
    public int WindowHeight { get; set; } = 1080;
    public int ResolutionScale { get; set; } = 2;
    public bool VSync { get; set; } = true;
    /// <summary>anisotropic_override: -1 game default, 0 off, 1 = 1x ... 5 = 16x.</summary>
    public int Anisotropic { get; set; } = 3;
    /// <summary>swap_post_effect: none, fxaa, fxaa_extreme.</summary>
    public string AntiAliasing { get; set; } = "none";
    /// <summary>present_effect: bilinear, cas, fsr.</summary>
    public string ScalingFilter { get; set; } = "bilinear";
    public bool KeepAspectRatio { get; set; } = true;
    /// <summary>renderer: xenia (Xenos emulation) or plume (native, experimental).</summary>
    public string Renderer { get; set; } = "xenia";
    /// <summary>plume_msaa: samples per pixel of the MSAA surfaces (native renderer).</summary>
    public int Msaa { get; set; } = 4;
    /// <summary>gpu_backend: d3d12 or vulkan.</summary>
    public string GraphicsApi { get; set; } = "d3d12";
    /// <summary>d3d12_adapter: -1 = automatic (high-performance GPU).</summary>
    public int GpuAdapter { get; set; } = -1;
    /// <summary>vulkan_device: -1 = automatic (dedicated GPU).</summary>
    public int VulkanDevice { get; set; } = -1;

    // Game
    /// <summary>user_language: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian.</summary>
    public int Language { get; set; } = 1;
    public string SaveFolder { get; set; } = "";
    public string GameFolder { get; set; } = "";
    public string GameExecutable { get; set; } = "";

    // Controls
    public bool KeyboardControls { get; set; } = true;
    public bool MouseLook { get; set; } = false;
    public double MouseSensitivity { get; set; } = 1.0;
    /// <summary>padbind_* option -> physical controller control(s); only non-default entries.</summary>
    public Dictionary<string, string> PadBinds { get; set; } = new();
    /// <summary>keybind_* option -> key(s); only non-default entries.</summary>
    public Dictionary<string, string> KeyBinds { get; set; } = new();

    // Cheats
    public bool UnlockAllCharacters { get; set; } = false;
    public bool UnlockAllCostumes { get; set; } = false;
    public bool BackupSaveBeforeCheats { get; set; } = true;

    // Advanced
    public string ExtraArguments { get; set; } = "";

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public static string FilePath => Path.Combine(AppPaths.DataFolder, "settings.json");

    /// <summary>Where older launcher builds kept the settings (next to the launcher).</summary>
    private static string LegacyFilePath => Path.Combine(AppContext.BaseDirectory, "hajinmon_settings.json");

    public static Settings Load()
    {
        try
        {
            if (!File.Exists(FilePath) && File.Exists(LegacyFilePath))
            {
                File.Copy(LegacyFilePath, FilePath);
            }
            if (File.Exists(FilePath))
            {
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath), JsonOptions)
                       ?? new Settings();
            }
        }
        catch (Exception)
        {
            // A corrupt settings file falls back to the defaults.
        }
        return new Settings();
    }

    public void Save() => File.WriteAllText(FilePath, JsonSerializer.Serialize(this, JsonOptions));
}
