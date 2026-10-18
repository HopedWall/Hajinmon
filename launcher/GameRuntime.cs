using System.IO.Compression;
using System.Reflection;

namespace Hajinmon.Launcher;

/// <summary>
/// The game runtime (HajinmonGame.exe and its DLLs) embedded in release builds of the
/// launcher. It contains no game data: the player points the launcher at their
/// own copy of the game. The runtime is extracted to the local app data folder
/// the first time it is needed, once per launcher build.
/// </summary>
public static class GameRuntime
{
    private const string ResourceName = "game_runtime.zip";

    public static bool IsEmbedded =>
        Assembly.GetExecutingAssembly().GetManifestResourceInfo(ResourceName) != null;

    private static string RuntimeRoot => Path.Combine(AppPaths.DataFolder, "runtime");

    /// <summary>Where this build's runtime lives once extracted.</summary>
    private static string Folder =>
        Path.Combine(RuntimeRoot, Assembly.GetExecutingAssembly().ManifestModule.ModuleVersionId.ToString("N"));

    /// <summary>Path of the embedded game executable (it may not be extracted yet).</summary>
    public static string ExecutablePath => Path.Combine(Folder, "HajinmonGame.exe");

    /// <summary>Extracts the embedded runtime if needed and returns the path of the game executable.</summary>
    public static string Extract()
    {
        string folder = Folder;
        string marker = Path.Combine(folder, ".complete");
        if (File.Exists(marker)) return ExecutablePath;

        string temp = folder + ".tmp";
        if (Directory.Exists(temp)) Directory.Delete(temp, true);
        using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName)!)
        using (var zip = new ZipArchive(stream, ZipArchiveMode.Read))
        {
            zip.ExtractToDirectory(temp);
        }
        File.WriteAllText(Path.Combine(temp, ".complete"), "");
        if (Directory.Exists(folder)) Directory.Delete(folder, true);
        Directory.Move(temp, folder);
        RemoveOldVersions(folder);
        return ExecutablePath;
    }

    /// <summary>Deletes runtimes extracted by older launcher builds.</summary>
    private static void RemoveOldVersions(string keep)
    {
        foreach (var dir in new DirectoryInfo(RuntimeRoot).EnumerateDirectories())
        {
            if (string.Equals(dir.FullName, keep, StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                dir.Delete(true);
            }
            catch (Exception)
            {
                // Still in use (an older game instance is running): try again next time.
            }
        }
    }
}

/// <summary>Per-user folder for the launcher's settings, backups and runtime.</summary>
public static class AppPaths
{
    public static string DataFolder
    {
        get
        {
            string folder = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Hajinmon");
            Directory.CreateDirectory(folder);
            return folder;
        }
    }
}
