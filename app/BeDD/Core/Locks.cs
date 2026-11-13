using Microsoft.Win32.SafeHandles;

namespace BeDD.Core;

// The machine-wide signaling lock. Every game on this PC signs in to Minecraft's online signaling
// service with the same ID (same PC, same account), and each new sign-in knocks the previous one
// out, so a run that joins a server online needs the PC to itself. A reader/writer lock held
// through file handles, which the OS releases when the holder dies (a killed run never leaves it
// stuck):
//   - every game holds signaling.lock SHARED (read access, read sharing) for its whole life;
//   - a game started with --exclusive-signaling holds it EXCLUSIVE (no sharing): new games wait,
//     and it waits for the running ones to close first;
//   - signaling.lock.wait is held without sharing by an exclusive run while it waits, and opened
//     for a moment by every shared one before it takes the lock, so new shared holders queue behind
//     a waiting exclusive run instead of starving it.
// The lock must live as long as the game, not the command that started it: Launcher hands Handle to
// the game process as an inherited handle (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), then this object is
// disposed and the game's copy of the handle keeps the lock.
public sealed class SignalingLock : IDisposable
{
    public const string FileName = "signaling.lock";

    private SafeFileHandle? _wait;

    private SignalingLock(SafeFileHandle handle, SafeFileHandle? wait, bool exclusive, string file)
    {
        Handle = handle;
        _wait = wait;
        Exclusive = exclusive;
        File = file;
    }

    // The lock file's handle; pass it to the game to hold the lock for the game's lifetime.
    public SafeFileHandle Handle { get; }
    public bool Exclusive { get; }
    public string File { get; }

    public static string PathIn(string? dir) => System.IO.Path.Combine(dir ?? Paths.Locks, FileName);

    // One attempt; null when the lock is not available now. For exclusive, a failed attempt keeps
    // nothing: the caller that wants to queue uses Acquire, which holds the .wait file between
    // attempts.
    public static SignalingLock? TryAcquire(bool exclusive, string? dir = null)
    {
        SafeFileHandle? wait = null;
        try
        {
            return TryAcquire(exclusive, dir, ref wait);
        }
        finally
        {
            wait?.Dispose();
        }
    }

    // Waits up to timeout for the lock. Exclusive: queues on the .wait file first, then waits for
    // the shared holders to close, then (othersRunning, when given) for games that do not hold the
    // lock at all, e.g. started by older tools. progress gets a line now and then while it waits.
    public static SignalingLock Acquire(bool exclusive, TimeSpan timeout, Action<string>? progress = null,
        string? dir = null, Func<IReadOnlyList<string>>? othersRunning = null, CancellationToken cancel = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        var nextReport = DateTime.UtcNow;
        string what = exclusive ? "exclusive" : "shared";
        SafeFileHandle? wait = null;
        try
        {
            while (true)
            {
                var got = TryAcquire(exclusive, dir, ref wait);
                if (got is not null)
                {
                    if (exclusive && othersRunning is not null)
                        WaitForOthers(got, deadline, progress, othersRunning, cancel);
                    wait = null; // owned by got now
                    return got;
                }
                if (DateTime.UtcNow >= deadline)
                    throw new TimeoutException($"timed out after {Format(timeout)} waiting for the {what} signaling lock ({PathIn(dir)})");
                if (DateTime.UtcNow >= nextReport)
                {
                    progress?.Invoke(exclusive
                        ? "waiting for the exclusive signaling lock: other games are still running"
                        : "waiting for the shared signaling lock: a game that joins online has the PC to itself");
                    nextReport = DateTime.UtcNow.AddSeconds(60);
                }
                if (cancel.WaitHandle.WaitOne(TimeSpan.FromMilliseconds(500)))
                    cancel.ThrowIfCancellationRequested();
            }
        }
        finally
        {
            wait?.Dispose();
        }
    }

    private static void WaitForOthers(SignalingLock got, DateTime deadline, Action<string>? progress,
        Func<IReadOnlyList<string>> othersRunning, CancellationToken cancel)
    {
        bool reported = false;
        while (true)
        {
            var others = othersRunning();
            if (others.Count == 0)
                return;
            if (DateTime.UtcNow >= deadline)
            {
                got.Dispose();
                throw new TimeoutException("timed out waiting for games outside the signaling lock to close (" + string.Join(", ", others) + ")");
            }
            if (!reported)
            {
                progress?.Invoke("exclusive signaling: waiting for games outside the lock to close (" + string.Join(", ", others) + ")");
                reported = true;
            }
            if (cancel.WaitHandle.WaitOne(TimeSpan.FromSeconds(2)))
            {
                got.Dispose();
                cancel.ThrowIfCancellationRequested();
            }
        }
    }

    private static SignalingLock? TryAcquire(bool exclusive, string? dir, ref SafeFileHandle? wait)
    {
        string file = PathIn(dir);
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(file)!);
        string waitFile = file + ".wait";
        try
        {
            if (exclusive)
            {
                // Block new shared holders first, then wait for the current ones to close.
                wait ??= System.IO.File.OpenHandle(waitFile, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                var h = System.IO.File.OpenHandle(file, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                var l = new SignalingLock(h, wait, true, file);
                wait = null;
                return l;
            }
            // Queue behind a waiting exclusive run: it holds the .wait file without sharing then.
            using (System.IO.File.OpenHandle(waitFile, FileMode.OpenOrCreate, FileAccess.Read, FileShare.ReadWrite))
            {
                var h = System.IO.File.OpenHandle(file, FileMode.OpenOrCreate, FileAccess.Read, FileShare.Read);
                return new SignalingLock(h, null, false, file);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // A sharing violation: someone holds it in a conflicting way. (UnauthorizedAccess shows
            // up while another process is deleting or creating the file.)
            return null;
        }
    }

    private static string Format(TimeSpan t) =>
        t.TotalHours >= 1 && t.TotalHours == Math.Floor(t.TotalHours) ? $"{t.TotalHours:0} h"
        : t.TotalMinutes >= 1 && t.TotalMinutes == Math.Floor(t.TotalMinutes) ? $"{t.TotalMinutes:0} min"
        : $"{t.TotalSeconds:0.#} s";

    // Releases this process's handles. A game that inherited Handle keeps the lock.
    public void Dispose()
    {
        Handle.Dispose();
        _wait?.Dispose();
        _wait = null;
    }
}
