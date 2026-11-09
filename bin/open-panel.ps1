param([int]$Port = 47901)

# Open the DevAI panel window and make sure it really shows up.
# 1) ask the local agent to open (or raise) the panel window
# 2) wait for a visible window titled "DevAI ..."
# 3) if nothing appears, launch the browser app window directly

$ErrorActionPreference = 'SilentlyContinue'
$DevAIHome = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$target = "http://127.0.0.1:$Port/"

function Send-ShowPanel {
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "http://127.0.0.1:$Port/api/show-panel" `
             -Body '{}' -ContentType 'application/json' -TimeoutSec 2
        return ($r.StatusCode -eq 200)
    } catch { return $false }
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DevAIPanelWindow {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static bool Has() {
        bool found = false;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            if (!IsWindowVisible(h)) return true;
            StringBuilder sb = new StringBuilder(256);
            GetWindowTextW(h, sb, 256);
            if (sb.ToString().StartsWith("DevAI")) { found = true; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

# 1) let the agent open it (retry while the service is still booting)
$ok = $false
for ($i = 0; $i -lt 20; $i++) {
    if (Send-ShowPanel) { $ok = $true; break }
    Start-Sleep -Milliseconds 700
}

# 2) wait for the window to become visible
for ($i = 0; $i -lt 24; $i++) {
    if ([DevAIPanelWindow]::Has()) { exit 0 }
    Start-Sleep -Milliseconds 350
}

# 3) still nothing: start the app window ourselves
$exes = @(
    'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
    'C:\Program Files\Google\Chrome\Application\chrome.exe',
    'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe'
)
$exe = $exes | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($exe) {
    Start-Process -FilePath $exe -ArgumentList @(
        "--app=$target",
        "--user-data-dir=$DevAIHome\panelprofile",
        '--window-size=460,960',
        '--window-position=1420,20',
        '--no-first-run',
        '--disable-features=CalculateNativeWinOcclusion,Translate,msEdgeIdentity'
    )
} else {
    Start-Process $target
}
exit 0
