<#
  带断点续传与重试的下载器。

  为什么需要它：
    网络不稳时，一次性拉一个大文件很容易半途断掉。而 GitHub 的 archive 链接
    （/archive/refs/heads/xxx.zip）是**服务端即时打包生成**的，不支持 HTTP Range
    —— 对它做断点续传是无效的。

    所以策略是：
      1. 优先**按单个文件**下载（raw.githubusercontent.com / jsDelivr 这类 CDN
         支持 Range），每个文件都小，失败重试代价低；
      2. 本脚本负责「续传 + 退避重试」，并在服务器不支持续传时自动从头再来。

  用法：
    .\fetch.ps1 -Url <url> -Out <path>
    .\fetch.ps1 -Url <url> -Out <path> -Proxy http://127.0.0.1:8902 -Retries 10
    .\fetch.ps1 -Url <url> -Out <path> -ExpectSize 2023776

  退出码：0 成功；非 0 失败（已用尽重试）。
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Url,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Proxy,
    [int]$Retries = 8,
    [int]$RetryDelaySec = 3,
    [long]$ExpectSize = 0,
    [switch]$Quiet
)

$ErrorActionPreference = 'Continue'

function Say($msg, $color = 'Gray') {
    if (-not $Quiet) { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg) -ForegroundColor $color }
}

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$proxyArgs = @()
if ($Proxy) { $proxyArgs = @('-x', $Proxy) }

for ($attempt = 1; $attempt -le $Retries; $attempt++) {
    $have   = if (Test-Path $Out) { (Get-Item $Out).Length } else { 0 }
    $resume = if ($have -gt 0) { @('-C', '-') } else { @() }

    Say ("第 {0}/{1} 次  已有 {2:N0} B  {3}" -f $attempt, $Retries, $have,
         (Split-Path -Leaf $Out)) Cyan

    $curlArgs = @() + $proxyArgs + @(
        '-L', '--fail', '--show-error',
        '--connect-timeout', '20',
        '--max-time', '900',
        '--retry', '3', '--retry-delay', '2', '--retry-all-errors',
        '-o', $Out
    ) + $resume + @($Url)

    $resp = & curl.exe @curlArgs 2>&1
    $code = $LASTEXITCODE

    if ($code -eq 0 -and (Test-Path $Out)) {
        $size = (Get-Item $Out).Length
        if ($ExpectSize -gt 0 -and $size -ne $ExpectSize) {
            Say ("  大小不符：期望 {0:N0}，实际 {1:N0} —— 视为失败" -f $ExpectSize, $size) Yellow
        } else {
            Say ("  完成：{0:N0} B" -f $size) Green
            return $Out
        }
    } else {
        Say ("  失败 curl exit={0}  {1}" -f $code, (($resp | Select-Object -First 2) -join ' ')) Yellow

        # 33 = HTTP_RANGE_ERROR，36 = BAD_DOWNLOAD_RESUME
        # 服务器不支持续传时，留着半成品只会一直失败，删掉重来。
        if ($code -eq 33 -or $code -eq 36) {
            Say "  服务器不支持续传，删除半成品后重下" Yellow
            Remove-Item $Out -Force -ErrorAction SilentlyContinue
        }
    }

    Start-Sleep -Seconds $RetryDelaySec
}

Say ("放弃：已重试 {0} 次" -f $Retries) Red
exit 1
