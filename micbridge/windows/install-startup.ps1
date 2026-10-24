param(
    [Parameter(Mandatory=$true)][System.Net.IPAddress]$ClientAddress,
    [ValidatePattern('^[^"\r\n]+$')][string]$OutputDevice = 'Steam Streaming Microphone'
)
$ErrorActionPreference = 'Stop'
if ($ClientAddress.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'ClientAddress must be the Linux client IPv4 address.'
}
$source = Join-Path $PSScriptRoot 'micbridge-receiver.exe'
if (!(Test-Path -LiteralPath $source)) { throw "Receiver missing: $source" }
$folder = Join-Path $env:LOCALAPPDATA 'MicBridge'
$target = Join-Path $folder 'micbridge-receiver.exe'
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$task = Get-ScheduledTask -TaskName 'MicBridge Receiver' -ErrorAction SilentlyContinue
if ($task) { Stop-ScheduledTask -TaskName 'MicBridge Receiver' }
Get-CimInstance Win32_Process -Filter "Name='micbridge-receiver.exe'" |
    Where-Object {$_.ExecutablePath -eq $target} |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
if ($source -ne $target) { Copy-Item -LiteralPath $source -Destination $target -Force }
@{ClientAddress=$ClientAddress.ToString(); OutputDevice=$OutputDevice} |
    ConvertTo-Json | Set-Content (Join-Path $folder 'config.json') -Encoding UTF8
$launcher = Join-Path $folder 'Start-Receiver.ps1'
@'
$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
try {
    $config = Get-Content (Join-Path $folder 'config.json') -Raw | ConvertFrom-Json
    $address = [System.Net.IPAddress]::Parse($config.ClientAddress)
    if ($address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { throw 'Invalid IPv4 address' }
    if ($config.OutputDevice -match '["\r\n]') { throw 'Invalid audio endpoint name' }
    $arguments = '--allow {0} --device "{1}"' -f $address.ToString(), $config.OutputDevice
    $process = Start-Process -FilePath (Join-Path $folder 'micbridge-receiver.exe') -ArgumentList $arguments -WorkingDirectory $folder -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $folder 'receiver.out.log') -RedirectStandardError (Join-Path $folder 'receiver.err.log')
    exit $process.ExitCode
} catch {
    $_ | Out-String | Add-Content (Join-Path $folder 'startup-error.log')
    exit 1
}
'@ | Set-Content -LiteralPath $launcher -Encoding UTF8
$ruleName = 'MicBridge-UDP-39842'
if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) {
    Remove-NetFirewallRule -Name $ruleName
}
New-NetFirewallRule -Name $ruleName -DisplayName 'MicBridge from Linux client' -Direction Inbound -Action Allow -Protocol UDP -LocalPort 39842 -RemoteAddress $ClientAddress.ToString() -Program $target -Profile Any | Out-Null
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}"' -f $launcher) -WorkingDirectory $folder
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$trigger.Delay = 'PT5S'
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'MicBridge Receiver' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description 'Receive Moonlight MicBridge audio into the host virtual microphone.' -Force | Out-Null
Start-ScheduledTask -TaskName 'MicBridge Receiver'
Write-Output "Installed MicBridge in $folder; starts silently at sign-in."
Write-Output 'Select Microphone (Steam Streaming Microphone) in your Windows voice app.'
