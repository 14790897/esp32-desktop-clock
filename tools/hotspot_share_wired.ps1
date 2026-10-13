# 让移动热点共享「有线」。前提：有线网卡已启用、插好网线、拿到了 IP。
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File hotspot_share_wired.ps1

$ErrorActionPreference = "Continue"
function Log($m) { Write-Output $m }

Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime] | Out-Null
[Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime] | Out-Null
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($t, $rt) {
    $asTask = $asTaskGeneric.MakeGenericMethod($rt)
    $netTask = $asTask.Invoke($null, @($t)); $netTask.Wait(-1) | Out-Null; $netTask.Result
}

# 这些是虚拟/隧道网卡，共享出去等于没网 —— 之前脚本就是错选了 SecureLink
$virtualNames = @('SecureLink', 'Tailscale', '蓝牙网络连接', 'vEthernet (Default Switch)')

# 候选 = 已启用、有链路、且有 IP 的物理网卡
$candidates = Get-NetAdapter | Where-Object {
    $_.Status -eq 'Up' -and $_.MediaConnectionState -eq 'Connected' -and $_.InterfaceDescription -notmatch 'Virtual|Tunnel|Bluetooth'
}
if (-not $candidates) { Log "!! 没有已连接的有线网卡 —— 先确认网线插好、网卡没被禁用"; exit 1 }

$wired = $candidates | Where-Object { $_.InterfaceDescription -notmatch 'Wi-Fi|Wireless|WLAN' } | Select-Object -First 1
if (-not $wired) { Log "!! 当前只有无线连着，没有可共享的有线"; exit 1 }

$alias = $wired.Name
$ip = (Get-NetIPAddress -InterfaceAlias $alias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1).IPAddress
Log "有线: $alias ($($wired.InterfaceDescription))  IP=$ip"
if (-not $ip) { Log "!! 有线没有拿到 IP，没法共享"; exit 1 }

# ConnectionProfile 的名字通常就是网卡别名，找不到就按描述兜底匹配
$profiles = [Windows.Networking.Connectivity.NetworkInformation]::GetConnectionProfiles()
$target = $profiles | Where-Object { $_.ProfileName -eq $alias -and -not $_.IsWlanConnectionProfile }
if (-not $target) {
    Log "可选的连接配置："
    $profiles | ForEach-Object { Log "    '$($_.ProfileName)'  WLAN=$($_.IsWlanConnectionProfile)" }
    Log "!! 没找到 '$alias' 对应的连接配置。改用设置界面选：设置 → 网络和 Internet → 移动热点 → 共享网络连接"
    exit 1
}

$mgr = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($target)
Log "热点原状态 = $($mgr.TetheringOperationalState)"
try { Await ($mgr.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult]) | Out-Null } catch {}
Start-Sleep -Seconds 3
$r = Await ($mgr.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
Log "热点已改为共享 '$alias'：$($r.Status)"
Start-Sleep -Seconds 6
Log "热点状态 = $($mgr.TetheringOperationalState)  SSID=$($mgr.GetCurrentAccessPointConfiguration().Ssid)"
