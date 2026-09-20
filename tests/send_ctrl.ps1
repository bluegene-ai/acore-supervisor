# send_ctrl.ps1 - drive the supervisor's console-control path from outside.
#   -Mode break : CTRL_BREAK_EVENT to every process sharing the target's console
#   -Mode close : post WM_CLOSE to the target's console window (= user clicks the X)
# Runs as its own process because a process can only be attached to one console at a time.
param(
    [Parameter(Mandatory = $true)][int]$TargetPid,
    [ValidateSet('break', 'close')][string]$Mode = 'break'
)

Add-Type -Namespace CtrlSend -Name N -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint dwProcessId);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(System.IntPtr handler, bool add);
[DllImport("kernel32.dll")] public static extern System.IntPtr GetConsoleWindow();
[DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessageW(System.IntPtr hWnd, uint msg, System.IntPtr w, System.IntPtr l);
'@

[void][CtrlSend.N]::FreeConsole()
if (-not [CtrlSend.N]::AttachConsole([uint32]$TargetPid)) { Write-Output 'attach failed'; exit 2 }

if ($Mode -eq 'break') {
    [void][CtrlSend.N]::SetConsoleCtrlHandler([System.IntPtr]::Zero, $true)
    $ok = [CtrlSend.N]::GenerateConsoleCtrlEvent(1, 0)     # 1 = CTRL_BREAK_EVENT, 0 = whole console
    Start-Sleep -Milliseconds 300
    if ($ok) { Write-Output 'ctrl-break delivered'; exit 0 } else { Write-Output 'ctrl-break refused'; exit 3 }
}

$hwnd = [CtrlSend.N]::GetConsoleWindow()
if ($hwnd -eq [System.IntPtr]::Zero) { Write-Output 'no console window to close'; exit 4 }
$ok = [CtrlSend.N]::PostMessageW($hwnd, 0x0010, [System.IntPtr]::Zero, [System.IntPtr]::Zero)   # WM_CLOSE
Start-Sleep -Milliseconds 300
if ($ok) { Write-Output 'wm_close posted'; exit 0 } else { Write-Output 'wm_close failed'; exit 5 }
