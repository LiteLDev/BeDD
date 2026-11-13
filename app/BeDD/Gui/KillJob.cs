using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.System.JobObjects;
using Windows.Win32.System.Threading;

namespace BeDD.Gui;

// A kill-on-close job holding one game: the game goes when the manager goes, even when the manager
// is killed. One job per game, because a game already in its starter's job can only join a job
// nested in that one, and two games started from different jobs cannot share a nested job.
internal sealed class KillJob : IDisposable
{
    private SafeFileHandle? _job;

    private KillJob(SafeFileHandle job) => _job = job;

    // The job, or null with the reason in note.
    public static unsafe KillJob? Create(int pid, out string note)
    {
        var job = PInvoke.CreateJobObject(null, null);
        if (job.IsInvalid)
        {
            note = "no job (" + Marshal.GetLastWin32Error() + ")";
            return null;
        }
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!PInvoke.SetInformationJobObject((Windows.Win32.Foundation.HANDLE)job.DangerousGetHandle(), JOBOBJECTINFOCLASS.JobObjectExtendedLimitInformation, &info,
                (uint)sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)))
        {
            note = "job setup failed (" + Marshal.GetLastWin32Error() + ")";
            job.Dispose();
            return null;
        }
        using var process = PInvoke.OpenProcess_SafeHandle(
            PROCESS_ACCESS_RIGHTS.PROCESS_SET_QUOTA | PROCESS_ACCESS_RIGHTS.PROCESS_TERMINATE, false, (uint)pid);
        if (process.IsInvalid)
        {
            note = "cannot open the process (" + Marshal.GetLastWin32Error() + ")";
            job.Dispose();
            return null;
        }
        if (!PInvoke.AssignProcessToJobObject(job, process))
        {
            note = "cannot join a job (" + Marshal.GetLastWin32Error() + ")";
            job.Dispose();
            return null;
        }
        note = "";
        return new KillJob(job);
    }

    // Closing the last handle kills the game if it still runs.
    public void Dispose()
    {
        Interlocked.Exchange(ref _job, null)?.Dispose();
    }
}
