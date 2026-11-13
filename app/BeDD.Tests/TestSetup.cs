using System.Runtime.CompilerServices;
using BeDD.Core;

namespace BeDD.Tests;

internal static class TestSetup
{
    // The tests' setup lock is their own: a BeDD setup running on the PC does not hold them up.
    [ModuleInitializer]
    internal static void Init() => SetupLock.MutexName = @"Local\BeDD.Setup.Tests-" + Environment.ProcessId;
}
