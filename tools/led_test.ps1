# bitEngine LED 测试脚本
# 用法: 双击 led_test.bat, 或 powershell -ExecutionPolicy Bypass -File led_test.ps1 [-Ip x.x.x.x]
param([string]$Ip = "192.168.1.14")

$uri = "http://$Ip/api/v1/led/test"
$ledNames = @("SYS", "Zigbee", "Thread", "Net")
$colors = [ordered]@{ "FF0000" = "红"; "00FF00" = "绿"; "0000FF" = "蓝" }

Write-Host "=== bitEngine LED 测试 ($Ip) ===" -ForegroundColor Cyan
Write-Host "预期: 每步只有一颗灯亮、颜色正确, 其余全灭`n"

$fail = 0
foreach ($i in 0..3) {
    foreach ($c in $colors.GetEnumerator()) {
        Write-Host ("[{0}/12] LED{1} ({2}) -> {3}" -f ($i*3 + [array]::IndexOf(@($colors.Keys), $c.Key) + 1), $i, $ledNames[$i], $c.Value)
        try {
            $r = Invoke-RestMethod -Method Post -Uri $uri -ContentType "application/json" `
                 -Body ('{"index":' + $i + ',"color":"' + $c.Key + '","hold_s":2}') -TimeoutSec 5
            if ($r.status -ne "ok") { Write-Host "  接口返回异常: $($r | ConvertTo-Json -Compress)" -ForegroundColor Red; $fail++ }
        } catch {
            Write-Host "  请求失败: $_" -ForegroundColor Red; $fail++
        }
        Start-Sleep -Milliseconds 2200
    }
}

Write-Host "`n[13/13] 全部灯珠白光 5 秒 (产测全亮项)"
try {
    Invoke-RestMethod -Method Post -Uri $uri -ContentType "application/json" `
        -Body '{"index":-1,"color":"FFFFFF","hold_s":5}' -TimeoutSec 5 | Out-Null
} catch { Write-Host "  请求失败: $_" -ForegroundColor Red; $fail++ }
Start-Sleep -Seconds 5

Write-Host ""
if ($fail -eq 0) {
    Write-Host "接口调用全部成功。请人工确认:" -ForegroundColor Green
    Write-Host "  1. 12 步中每步只有一颗灯亮且颜色正确 -> 单灯单色全部可控"
    Write-Host "  2. 某灯红绿颠倒 -> 灯珠 RGB/GRB 顺序问题(可固件修正)"
    Write-Host "  3. 从某颗灯起全部不亮 -> 菊花链在该灯处断裂"
} else {
    Write-Host "$fail 个请求失败 — 检查板子 IP / 网络连通性" -ForegroundColor Red
}
Read-Host "`n按回车退出"
