<#
  tools/factory_test.ps1 —— 产线整机自检（Windows 版，一块板一次，约 1 分钟）

    powershell -ExecutionPolicy Bypass -File factory_test.ps1 -GW 192.168.1.50
    powershell -ExecutionPolicy Bypass -File factory_test.ps1 -GW 192.168.1.50 -Auto
    powershell -ExecutionPolicy Bypass -File factory_test.ps1 -GW 192.168.1.50 -Pass 密码
    powershell -ExecutionPolicy Bypass -File factory_test.ps1 -GW 192.168.1.50 -Power usb
    powershell -ExecutionPolicy Bypass -File factory_test.ps1 -GW 192.168.1.50 -Retest

  这是 tools/factory_test.sh 的 Windows 移植。行为必须与 .sh 保持一致 ——
  两边同时改，否则产线在两个系统上会得到不同的放行标准。

  供电方式固件判不出来：没检测到 PoE 时，"这块板没用 PoE 供电"和"PoE 分压电路坏了"
  数字读数完全相同（poe.h 有实测表）。所以由本脚本按 -Power 告诉它该怎么算：
    -Power poe（默认）  未检测到 PoE = 不合格
    -Power usb          未检测到 PoE = 正常，按"不适用"消掉
  -Retest 允许同一块 MAC 再测一次（返修复测）。

  ⚠️ 不依赖 python / curl / bash：产线的 Windows 机器上通常一个都没有。
     只用 PowerShell 5.1 自带的 Invoke-RestMethod。

  设计原则见 components/webui/factory_api.c 顶部：判据集中在固件里，本脚本只负责
  驱动流程和收口。**manual 项绝不算 pass** —— 整机只有零 fail、零 manual 才放行。

  前置：网线插好、板子已上电并拿到 IP。出厂态无密码，直接可调。
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GW,
    [switch]$Auto,
    [string]$Pass = $env:GW_PASS,
    # ValidateSet：打错一个字会直接报错而不是静默按默认值跑 —— -Power 打错的后果
    # 是 PoE 故障被当成正常放行，这种错必须吵。
    [ValidateSet('poe','usb')][string]$Power = 'poe',
    [switch]$Retest
)

$ErrorActionPreference = 'Stop'
# 产线机器多半没配 TLS/代理，且这里全是明文 HTTP 局域网请求
[Net.ServicePointManager]::Expect100Continue = $false

# 终端输出按 UTF-8。⚠️ 这一行只管**终端**，不管 Invoke-RestMethod 怎么解码
# **响应体**：5.1 解响应体看的是 Content-Type 里的 charset，没有就按 ISO-8859-1，
# detail 里的中文会成乱码 —— 产线看不懂就等于没写。所以固件侧 JSON 回包统一带
# `charset=utf-8`（webui.h 的 WEBUI_JSON_CONTENT_TYPE，2026-09-13），
# 这才是真正让 5.1 解对的那一处；本行只是让解对了的字能在终端显示出来。
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$Headers = @{}
if ($Pass) {
    # 用户名留空，固件只验冒号后那半（auth.c 的 check_basic_auth）
    $b = [Text.Encoding]::UTF8.GetBytes(":$Pass")
    $Headers['Authorization'] = 'Basic ' + [Convert]::ToBase64String($b)
}

$Self = "http://$GW/api/v1/factory/selftest"
$Ack  = "http://$GW/api/v1/factory/ack"

function Invoke-GW {
    param([string]$Url, [string]$Method = 'GET', $Body = $null)
    $p = @{ Uri = $Url; Method = $Method; TimeoutSec = 15; Headers = $Headers }
    if ($null -ne $Body) {
        $p['Body']        = ($Body | ConvertTo-Json -Compress)
        $p['ContentType'] = 'application/json'
    }
    Invoke-RestMethod @p
}

# ---- 401 预检：必须在任何断言之前 -----------------------------------------
# Invoke-RestMethod 收到 401 会抛异常，抓下来给一句人话，
# 而不是让操作员看一堆 .NET 栈。
# ⚠️ 探针必须打**无条件** AUTH_REQUIRED 的端点。此前探的是 /api/v1/status ——
#    它是 AUTH_REQUIRED_UNLESS_OPEN，rest_auth 出厂关时无凭据也回 200，于是预检
#    永远"通过"，第一条真请求 Get-Report 才 401，操作员看到的正是那堆 .NET 栈
#    （2026-09-13 用 pwsh 首次真跑本脚本发现）。/api/v1/ntp 与前端门禁探针同款；
#    tools/check_factory_test_parity.py 钉着"两边同一端点 + 固件侧确为无条件 AUTH_REQUIRED"。
try {
    $null = Invoke-GW "http://$GW/api/v1/ntp"
} catch {
    $code = $null
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    if ($code -eq 401) {
        Write-Host "[X] 需要认证（HTTP 401）。设备设了密码，请带上凭据重跑：" -ForegroundColor Red
        Write-Host "      powershell -ExecutionPolicy Bypass -File factory_test.ps1 -GW $GW -Pass <设备密码>"
    } else {
        Write-Host "[X] 连不上 $GW —— 网线/供电/IP 是否正确？" -ForegroundColor Red
        Write-Host "    $($_.Exception.Message)"
    }
    exit 1
}

function Get-Report { Invoke-GW $Self }

function Show-Report {
    param($d)
    $mark = @{ 'pass' = '[OK]  '; 'fail' = '[X]   '; 'manual' = '[人工]' }
    $col  = @{ 'pass' = 'Green';  'fail' = 'Red';    'manual' = 'Yellow' }
    foreach ($i in $d.items) {
        $m = $mark[$i.state]; if (-not $m) { $m = '[?]   ' }
        $c = $col[$i.state];  if (-not $c) { $c = 'Gray' }
        Write-Host ("  {0} {1,-14} {2}" -f $m, $i.name, $i.detail) -ForegroundColor $c
    }
    $acked = 0; if ($null -ne $d.acked) { $acked = $d.acked }
    Write-Host ""
    Write-Host ("  verdict = {0}   (fail={1}, manual={2}, acked={3})" -f $d.verdict, $d.fail, $d.manual, $acked)
}

function Get-ItemState {
    param($d, [string]$Name)
    ($d.items | Where-Object { $_.name -eq $Name } | Select-Object -First 1).state
}

# 把人工判定回写设备。此前操作员在终端里答了 y，那个答案从不回写 ——
# 于是 led_visual 永远是 manual，verdict 的 pass 分支是死分支，本脚本必然失败。
function Set-Ack {
    param([string]$Item, [string]$Result)
    try {
        $null = Invoke-GW $Ack 'POST' @{ item = $Item; result = $Result }
        return $true
    } catch {
        Write-Host "  [!] 回写 $Item=$Result 失败 —— 固件是否为 2026-08-17 之后的版本？" -ForegroundColor Yellow
        Write-Host "      旧固件没有 /api/v1/factory/ack，人工项永远消不掉、verdict 停在 incomplete。" -ForegroundColor Yellow
        return $false
    }
}

# 同一块板重测时，先清掉上一轮的确认，否则会把上次的 y 当成这次的
try { $null = Invoke-GW $Ack 'POST' @{ clear = $true } } catch { }

# ===========================================================================
Write-Host ""
Write-Host "=== 1. 自动项 ===" -ForegroundColor Cyan
$d = Get-Report
Show-Report $d

if ($d.fail -gt 0) {
    Write-Host ""
    Write-Host "[X] 有自动项不通过，停止 —— 上面 detail 里写了是什么值不满足什么阈值" -ForegroundColor Red
    exit 1
}

# ---- 1b. MAC 查重 ----------------------------------------------------------
# 固件侧**不存**全局表（它只知道自己那一块），查重只能在产线这一侧做。
# 拦两件事：同一块板被当成两块走完产线（返修板混进良品流），以及真的撞了 MAC。
#
# ⚠️ 台账**必须备份**，丢了就静默失去查重能力：脚本照跑，只是再也查不出重复。
# ⚠️ 与 generate_nvs.py 的身份台账是两回事：那份是"写了什么身份"的正本，
#    这份是"测过哪些板"的流水（返修板会重测但不重写身份）。
Write-Host ""
Write-Host "=== 1b. MAC 查重 ===" -ForegroundColor Cyan
$Ledger = Join-Path $PSScriptRoot '.factory_test_log.csv'
$idDetail = ($d.items | Where-Object { $_.name -eq 'identity' } | Select-Object -First 1).detail
$mMac = [regex]::Match($idDetail, 'mac=([0-9A-Fa-f]{12})')
$mSn  = [regex]::Match($idDetail, 'SN=(\S+)')
if (-not $mMac.Success) {
    Write-Host "  [X] 读不到 MAC —— identity 项的 detail 变了格式？固件与本脚本要成对更新" -ForegroundColor Red
    exit 1
}
$Mac = $mMac.Groups[1].Value.ToUpper()
$Sn  = if ($mSn.Success) { $mSn.Groups[1].Value } else { '-' }

if (-not (Test-Path $Ledger)) {
    Set-Content -Path $Ledger -Value 'utc_time,mac,sn,verdict,note' -Encoding ASCII
}
$seen = @(Import-Csv $Ledger | Where-Object { $_.mac -eq $Mac })
$Note = 'new'
if ($seen.Count -gt 0) {
    if ($Retest) {
        Write-Host "  [重测] $Mac 此前测过 —— 按 -Retest 放行（会另记一行 note=retest）" -ForegroundColor Yellow
        $Note = 'retest'
    } else {
        Write-Host "  [X] MAC $Mac 在台账里已经出现过：" -ForegroundColor Red
        foreach ($r in $seen) { Write-Host ("      {0},{1},{2},{3},{4}" -f $r.utc_time,$r.mac,$r.sn,$r.verdict,$r.note) }
        Write-Host "      要么这块板已经走过产线（返修复测请加 -Retest），"
        Write-Host "      要么两块板撞了 MAC —— 后者必须查清，别放行"
        exit 1
    }
} else {
    Write-Host "  [OK] MAC $Mac 未出现过（SN=$Sn）" -ForegroundColor Green
}

if ($Auto) {
    Write-Host ""
    Write-Host "[跳过] -Auto：不做人工确认项。注意 verdict 仍是 $($d.verdict)，**不等于整机通过**" -ForegroundColor Yellow
    exit 0
}

# ---- 2. LED 目视 -----------------------------------------------------------
Write-Host ""
Write-Host "=== 2. LED 目视（唯一判据是四颗全白）===" -ForegroundColor Cyan
# 判据必须是白色：WS2812 线序是 GRB，红在线上是 00 FF 00，第一个非零字节之后本来
# 就没有内容 —— 而 T1H 过大的失效恰恰是"该字节之后全部作废"，单色对它天然失明。
try {
    $null = Invoke-GW "http://$GW/api/v1/led/test" 'POST' @{ index = -1; color = 'FFFFFF'; hold_s = 5 }
} catch {
    Write-Host "  [!] 点灯请求失败，仍请肉眼确认" -ForegroundColor Yellow
}
Write-Host "  四颗灯已点白 5 秒。"
$a = Read-Host "  四颗是否**全部纯白**（不是绿/黄/偏色，末端那颗也要亮）？[y/N]"
if ($a -match '^[yY]') {
    $null = Set-Ack 'led_visual' 'pass'
    Write-Host "  [OK] LED 目视通过（已回写设备）" -ForegroundColor Green
} else {
    $null = Set-Ack 'led_visual' 'fail'
    Write-Host "  [X] LED 目视不通过 —— 查 T0H/T1H，见 docs/hardware 的 X5" -ForegroundColor Red
    Write-Host "      （已回写 fail，设备侧 verdict 也会是 fail，不会留下「没测」的假象）"
    exit 1
}

# ---- WiFi：产线不配 WiFi 时它是 manual，不处理它 = 永远 incomplete ----------
$d = Get-Report
if ((Get-ItemState $d 'wifi') -eq 'manual') {
    Write-Host ""
    $w = Read-Host "  WiFi 未连接。本产线是否**不配 WiFi**（跳过该项）？[y/N]"
    if ($w -match '^[yY]') {
        $null = Set-Ack 'wifi' 'pass'
        Write-Host "  [跳过] WiFi 按「产线不测」记为通过" -ForegroundColor Yellow
    } else {
        $null = Set-Ack 'wifi' 'fail'
        Write-Host "  [X] WiFi 应连未连 —— 查天线/凭据" -ForegroundColor Red
        exit 1
    }
}

# ---- wifi_scan：产线没有 5GHz AP 环境时扫到 0 个是正常的；现场确有 AP 却扫不到
#      就是这块板的射频有问题 —— 两者只有操作员分得清 -------------------------
$d = Get-Report
if ((Get-ItemState $d 'wifi_scan') -eq 'manual') {
    Write-Host ""
    $sc = Read-Host "  扫不到 5GHz AP。本产线**周围确实没有** 5GHz AP？[y/N]"
    if ($sc -match '^[yY]') {
        $null = Set-Ack 'wifi_scan' 'pass'
        Write-Host "  [跳过] wifi_scan 按「无 5GHz 环境」记为通过" -ForegroundColor Yellow
    } else {
        $null = Set-Ack 'wifi_scan' 'fail'
        Write-Host "  [X] 有 5GHz AP 却扫不到 —— 查 WiFi 天线/射频" -ForegroundColor Red
        exit 1
    }
}

# ---- PoE：固件分不出"没用 PoE 供电"和"PoE 分压坏了"，由 -Power 决定怎么算 ----
$d = Get-Report
if ((Get-ItemState $d 'poe') -eq 'manual') {
    Write-Host ""
    if ($Power -eq 'usb') {
        $null = Set-Ack 'poe' 'pass'
        Write-Host "  [跳过] PoE 未检测到，按 -Power usb 记为不适用" -ForegroundColor Yellow
    } else {
        $null = Set-Ack 'poe' 'fail'
        Write-Host "  [X] 按 PoE 供电却检测不到（IO0 为低）—— 查 TMI7321 与 R96/R97 分压" -ForegroundColor Red
        Write-Host "      若这台确实是 USB 供电，请加 -Power usb 重跑"
        exit 1
    }
}

# ---- 3. 按键 ---------------------------------------------------------------
Write-Host ""
Write-Host "=== 3. 按键 ===" -ForegroundColor Cyan
$d = Get-Report
if ((Get-ItemState $d 'button') -eq 'pass') {
    Write-Host "  [OK] 本次上电已记录到按压，跳过" -ForegroundColor Green
} else {
    Write-Host "  请按住按钮约 5 秒后松开。"
    Write-Host "  （2-10 秒落在保护带，不会重启、不会擦配置；红灯亮起是正常的进度提示）"
    $deadline = (Get-Date).AddSeconds(120)
    while ($true) {
        $d = Get-Report
        if ((Get-ItemState $d 'button') -eq 'pass') { break }
        if ((Get-Date) -ge $deadline) {
            Write-Host "  [X] 120 秒内没检测到按压 —— 查 IO26 焊接与上拉" -ForegroundColor Red
            exit 1
        }
        Start-Sleep -Seconds 3
    }
    Write-Host "  [OK] 按压已记录" -ForegroundColor Green
}

# 时长与分档必须落在保护带：证明的是"按下去有反应"且"没触发动作"
$btn = $d.items | Where-Object { $_.name -eq 'button' } | Select-Object -First 1
Write-Host "     $($btn.detail)"
$m = [regex]::Match($btn.detail, '(\d+) ms . (\w+)')
if ($m.Success) {
    $ms  = [int]$m.Groups[1].Value
    $act = $m.Groups[2].Value
    $ok  = ($ms -ge 2000 -and $ms -lt 10000 -and $act -eq 'none')
    $tag = if ($ok) { '[OK]' } else { '[X] ' }
    $c   = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("     {0} 时长 {1}ms / 动作 {2}（期望 2000-10000ms 且 none）" -f $tag, $ms, $act) -ForegroundColor $c
    if (-not $ok) {
        Write-Host "  [X] 按压时长或分档不对" -ForegroundColor Red
        exit 1
    }
}

# ---- 结果 ------------------------------------------------------------------
Write-Host ""
Write-Host "=== 结果 ===" -ForegroundColor Cyan
$d = Get-Report
if ($d.verdict -eq 'pass') {
    # 只有**通过**才记台账。不合格的板子会返修后重测，那次才是它真正走产线的一次；
    # 每次尝试都记 = 返修板第二次必然撞查重，把一条真判据变成天天喊狼来了。
    $line = '{0},{1},{2},{3},{4}' -f (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'), $Mac, $Sn, $d.verdict, $Note
    Add-Content -Path $Ledger -Value $line -Encoding ASCII
    Write-Host "  [OK] 整机通过（已记台账：$Mac $Sn）" -ForegroundColor Green
    exit 0
} else {
    Write-Host "  [!] verdict=$($d.verdict) —— 自动项与人工项都过了，但仍有 manual 未消。逐项看：" -ForegroundColor Yellow
    Show-Report $d
    exit 1
}
