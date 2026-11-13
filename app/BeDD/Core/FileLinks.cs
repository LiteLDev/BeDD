using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.Storage.FileSystem;

namespace BeDD.Core;

// Hard links, junctions and a tree delete that never follows a link: what a clone is made of.
public static class FileLinks
{
    // Win32 errors after which a hard link cannot be made here at all, so a copy is made instead:
    // another volume, a file system without hard links (FAT, some network shares), the 1023-link cap.
    public const int ErrorInvalidFunction = 1;
    public const int ErrorNotSameDevice = 17;
    public const int ErrorNotSupported = 50;
    public const int ErrorTooManyLinks = 1142;

    public static bool IsNoLinkError(int error) =>
        error is ErrorInvalidFunction or ErrorNotSameDevice or ErrorNotSupported or ErrorTooManyLinks;

    // 0 on success, else the Win32 error.
    public static int TryHardLink(string link, string existing) =>
        PInvoke.CreateHardLink(link, existing) ? 0 : Marshal.GetLastWin32Error();

    public static bool IsLink(FileSystemInfo info) => info.Attributes.HasFlag(FileAttributes.ReparsePoint);

    // The folder a junction (or directory symlink) points to, or null when path is no link.
    public static string? LinkTarget(string path)
    {
        var info = new DirectoryInfo(path);
        if (!info.Exists || !IsLink(info))
            return null;
        try
        {
            var target = info.LinkTarget;
            if (target is null)
                return null;
            if (target.StartsWith(@"\??\", StringComparison.Ordinal))
                target = target[4..];
            return Path.GetFullPath(target);
        }
        catch (IOException)
        {
            return null;
        }
    }

    // A junction at link (which must not exist) pointing to the folder target.
    public static void CreateJunction(string link, string target)
    {
        target = Path.GetFullPath(target).TrimEnd('\\');
        Directory.CreateDirectory(link);
        try
        {
            SetMountPoint(link, target);
        }
        catch (Win32Exception first)
        {
            // Fall back to cmd's mklink /J (it does the same, but some file system filters treat it
            // differently).
            Directory.Delete(link);
            if (!Mklink(link, target))
                throw new IOException($"could not create the junction {link} -> {target}: {first.Message}", first);
        }
    }

    private static unsafe void SetMountPoint(string link, string target)
    {
        byte[] sub = Encoding.Unicode.GetBytes(@"\??\" + target);
        byte[] print = Encoding.Unicode.GetBytes(target);
        int pathBytes = sub.Length + 2 + print.Length + 2;
        int dataLength = 8 + pathBytes;
        var buf = new byte[8 + dataLength];
        BitConverter.TryWriteBytes(buf.AsSpan(0), 0xA0000003u); // IO_REPARSE_TAG_MOUNT_POINT
        BitConverter.TryWriteBytes(buf.AsSpan(4), (ushort)dataLength);
        BitConverter.TryWriteBytes(buf.AsSpan(8), (ushort)0); // SubstituteNameOffset
        BitConverter.TryWriteBytes(buf.AsSpan(10), (ushort)sub.Length);
        BitConverter.TryWriteBytes(buf.AsSpan(12), (ushort)(sub.Length + 2)); // PrintNameOffset
        BitConverter.TryWriteBytes(buf.AsSpan(14), (ushort)print.Length);
        sub.CopyTo(buf, 16);
        print.CopyTo(buf, 16 + sub.Length + 2);

        // DeviceIoControl is called on the raw handle; h keeps it alive and closes it.
        using SafeFileHandle h = PInvoke.CreateFile(link, 0x40000000u /* GENERIC_WRITE */,
            FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE | FILE_SHARE_MODE.FILE_SHARE_DELETE, null,
            FILE_CREATION_DISPOSITION.OPEN_EXISTING,
            FILE_FLAGS_AND_ATTRIBUTES.FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAGS_AND_ATTRIBUTES.FILE_FLAG_OPEN_REPARSE_POINT, null);
        if (h.IsInvalid)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        fixed (byte* p = buf)
        {
            uint returned;
            if (!PInvoke.DeviceIoControl((HANDLE)h.DangerousGetHandle(), PInvoke.FSCTL_SET_REPARSE_POINT, p, (uint)buf.Length, null, 0, &returned, null))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }
    }

    private static bool Mklink(string link, string target)
    {
        var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"))
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        psi.ArgumentList.Add("/d");
        psi.ArgumentList.Add("/c");
        psi.ArgumentList.Add("mklink");
        psi.ArgumentList.Add("/J");
        psi.ArgumentList.Add(link);
        psi.ArgumentList.Add(target);
        using var p = Process.Start(psi);
        if (p is null)
            return false;
        p.StandardOutput.ReadToEnd();
        p.StandardError.ReadToEnd();
        p.WaitForExit();
        return p.ExitCode == 0 && LinkTarget(link) is not null;
    }

    // Removes a junction or link itself, never what it points to.
    public static void RemoveLink(string path)
    {
        var info = new DirectoryInfo(path);
        if (info.Exists)
            Directory.Delete(path, recursive: false); // RemoveDirectory drops the reparse point only
        else
            File.Delete(path);
    }

    // Deletes a folder tree. Links inside it (junctions to the base's data, symlinks) are removed as
    // links: their targets are never entered. Read-only files are deleted too. The entry named last
    // (a clone's .bedd) goes after everything else, so that a delete that fails halfway leaves a
    // folder that is still recognised and can be removed again.
    public static void DeleteTree(string path, string? last = null)
    {
        var info = new DirectoryInfo(path);
        if (!info.Exists)
        {
            if (File.Exists(path))
                DeleteFile(path);
            return;
        }
        if (IsLink(info))
        {
            RemoveLink(path);
            return;
        }
        var entries = info.EnumerateFileSystemInfos("*", new EnumerationOptions { AttributesToSkip = 0, IgnoreInaccessible = false })
            .OrderBy(e => last is not null && string.Equals(e.Name, last, StringComparison.OrdinalIgnoreCase) ? 1 : 0)
            .ToList();
        foreach (var entry in entries)
        {
            if (entry is DirectoryInfo d)
            {
                if (IsLink(d))
                    RemoveLink(d.FullName);
                else
                    DeleteTree(d.FullName);
            }
            else
            {
                DeleteFile(entry.FullName);
            }
        }
        if (info.Attributes.HasFlag(FileAttributes.ReadOnly))
            info.Attributes &= ~FileAttributes.ReadOnly;
        Directory.Delete(path, recursive: false);
    }

    // Deletes one file name (for a hard link: only this name; the base keeps its file). A read-only
    // file is deleted without clearing its read-only attribute, which a hard link shares with the
    // base's name: POSIX delete with FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE.
    public static void DeleteFile(string path)
    {
        if (!File.GetAttributes(path).HasFlag(FileAttributes.ReadOnly))
        {
            File.Delete(path);
            return;
        }
        DeleteIgnoringReadOnly(path);
    }

    private static unsafe void DeleteIgnoringReadOnly(string path)
    {
        const uint Delete = 0x00010000; // DELETE
        using SafeFileHandle h = PInvoke.CreateFile(path, Delete,
            FILE_SHARE_MODE.FILE_SHARE_READ | FILE_SHARE_MODE.FILE_SHARE_WRITE | FILE_SHARE_MODE.FILE_SHARE_DELETE, null,
            FILE_CREATION_DISPOSITION.OPEN_EXISTING, FILE_FLAGS_AND_ATTRIBUTES.FILE_FLAG_OPEN_REPARSE_POINT, null);
        if (h.IsInvalid)
            throw new IOException($"could not delete {path}: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");
        var info = new FILE_DISPOSITION_INFO_EX
        {
            Flags = FILE_DISPOSITION_INFO_EX_FLAGS.FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_INFO_EX_FLAGS.FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                FILE_DISPOSITION_INFO_EX_FLAGS.FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE,
        };
        if (!PInvoke.SetFileInformationByHandle((HANDLE)h.DangerousGetHandle(), FILE_INFO_BY_HANDLE_CLASS.FileDispositionInfoEx, &info,
                (uint)sizeof(FILE_DISPOSITION_INFO_EX)))
            throw new IOException($"could not delete {path}: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");
    }

    // Copies a folder tree; links inside are skipped (the caller's folders hold none).
    public static long CopyTree(string from, string to)
    {
        long bytes = 0;
        Directory.CreateDirectory(to);
        foreach (var entry in new DirectoryInfo(from).EnumerateFileSystemInfos("*", new EnumerationOptions { AttributesToSkip = 0 }))
        {
            string dest = Path.Combine(to, entry.Name);
            if (entry is DirectoryInfo d)
            {
                if (!IsLink(d))
                    bytes += CopyTree(d.FullName, dest);
            }
            else if (entry is FileInfo f)
            {
                f.CopyTo(dest, overwrite: true);
                bytes += f.Length;
            }
        }
        return bytes;
    }

    // Writes a file by writing a temporary one next to it and moving it over the old one: never
    // writes into an existing file (which could be a hard link to someone else's).
    public static void ReplaceFile(string path, byte[] content)
    {
        string tmp = path + ".bedd-tmp";
        File.WriteAllBytes(tmp, content);
        File.Move(tmp, path, overwrite: true);
    }

    // Full paths of the running processes whose image lies inside dir.
    public static List<int> ProcessesIn(string dir, string imageName = "Minecraft.Windows")
    {
        string prefix = Path.GetFullPath(dir).TrimEnd('\\') + "\\";
        var pids = new List<int>();
        foreach (var p in Process.GetProcessesByName(imageName))
        {
            using (p)
            {
                var image = ImagePath(p.Id);
                if (image is not null && image.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    pids.Add(p.Id);
            }
        }
        return pids;
    }

    private static string? ImagePath(int pid)
    {
        using var h = PInvoke.OpenProcess_SafeHandle(Windows.Win32.System.Threading.PROCESS_ACCESS_RIGHTS.PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)pid);
        if (h.IsInvalid)
            return null;
        Span<char> buf = stackalloc char[1024];
        uint size = (uint)buf.Length;
        if (!PInvoke.QueryFullProcessImageName(h, Windows.Win32.System.Threading.PROCESS_NAME_FORMAT.PROCESS_NAME_WIN32, buf, ref size))
            return null;
        return new string(buf[..(int)size]);
    }
}
