using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

// Read-only Win32 experiment. Run through the PowerShell process watchdog.
public static class DirectoryRegistrationProbe
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr FindFirstChangeNotificationW(string path, bool subtree, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FindCloseChangeNotification(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    public sealed class Result
    {
        public string Directory;
        public int RequestedRegistrations;
        public int SuccessfulRegistrations;
        public int RegistrationError;
        public double FirstMs;
        public double MedianMs;
        public double P95Ms;
        public double MaxMs;
        public int LifetimeTrials;
        public int QuietWhileThreadAlive;
        public int SignaledAfterThreadExit;
        public int PairedQuietThenSignaled;
    }

    static IntPtr Open(string path)
    {
        var handle = FindFirstChangeNotificationW(path, false, 0x17f);
        if (handle == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return handle;
    }

    static void Close(IntPtr handle)
    {
        if (!FindCloseChangeNotification(handle))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    public static Result Run(string path, int count, int trials)
    {
        var result = new Result { Directory = path, RequestedRegistrations = count };
        var times = new double[count];
        for (int i = 0; i < count; ++i)
        {
            var timer = Stopwatch.StartNew();
            IntPtr handle;
            try { handle = Open(path); }
            catch (Win32Exception error)
            {
                timer.Stop();
                result.RegistrationError = error.NativeErrorCode;
                result.MaxMs = Math.Max(result.MaxMs, timer.Elapsed.TotalMilliseconds);
                if (i == 0) result.FirstMs = timer.Elapsed.TotalMilliseconds;
                return result;
            }
            timer.Stop();
            times[i] = timer.Elapsed.TotalMilliseconds;
            if (i == 0) result.FirstMs = times[i];
            result.MaxMs = Math.Max(result.MaxMs, times[i]);
            Close(handle);
            ++result.SuccessfulRegistrations;
        }
        Array.Sort(times);
        result.MedianMs = times[count / 2];
        result.P95Ms = times[(int)Math.Ceiling(count * 0.95) - 1];

        for (int i = 0; i < trials; ++i)
        {
            IntPtr handle = IntPtr.Zero;
            Exception failure = null;
            using (var ready = new ManualResetEventSlim(false))
            using (var release = new ManualResetEventSlim(false))
            {
                var thread = new Thread(() =>
                {
                    try { handle = Open(path); }
                    catch (Exception error) { failure = error; }
                    finally { ready.Set(); }
                    release.Wait();
                });
                thread.IsBackground = true;
                thread.Start();
                // Parent process bounds even a native registration that never returns.
                ready.Wait();
                uint before = 0xffffffff;
                try
                {
                    if (failure != null) throw failure;
                    before = WaitForSingleObject(handle, 50);
                }
                finally { release.Set(); thread.Join(); }
                if (failure != null) throw failure;
                try
                {
                    if (before == 0xffffffff) throw new InvalidOperationException("Wait failed while registration thread was alive.");
                    uint after = WaitForSingleObject(handle, 500);
                    if (after == 0xffffffff) throw new Win32Exception(Marshal.GetLastWin32Error());
                    ++result.LifetimeTrials;
                    if (before == 258) ++result.QuietWhileThreadAlive;
                    if (after == 0) ++result.SignaledAfterThreadExit;
                    if (before == 258 && after == 0) ++result.PairedQuietThenSignaled;
                }
                finally { Close(handle); }
            }
        }
        return result;
    }
}
