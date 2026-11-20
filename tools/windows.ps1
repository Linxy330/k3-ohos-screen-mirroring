#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('1080p', '720p')][string] $Resolution,
    [Parameter(Position=1)][ValidateSet('hw', 'sw')][string] $Codec,
    [string] $HdcPath = $env:HDC,
    [switch] $RebootIfNeeded
)

$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$boardRoot = '/data/local/tmp/k3-virtual-main'
$uuidPattern = '[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}'
$script:stage = ''
$script:target = ''
$script:useAsciiTransfer = $false
# Only file-send needs the package cwd. Other commands can run from a short
# directory even when the user's extracted path exceeds the old client's limit.
$shellDirectory = if ($env:OS -eq 'Windows_NT' -and [Environment]::SystemDirectory) {
    [Environment]::SystemDirectory
} else { $packageRoot }

function Read-MenuChoice($Title, $First, $Second) {
    Write-Host "$Title"
    Write-Host "  1) $First（默认）"
    Write-Host "  2) $Second"
    while ($true) {
        $choice = Read-Host '选择 1 或 2，输入 q 退出'
        if ($null -eq $choice -or $choice.Trim() -eq 'q') { return 'q' }
        switch ($choice.Trim()) {
            ''  { return '1' }
            '1' { return '1' }
            '2' { return '2' }
        }
        Write-Host '请输入 1、2 或 q。'
    }
}

function Find-Hdc([string] $Requested) {
    if ($Requested) {
        if (Test-Path -LiteralPath $Requested -PathType Leaf) {
            return (Get-Item -LiteralPath $Requested).FullName
        }
        throw "找不到指定的 HDC 文件：$Requested"
    }

    # The portable and installed HongJing clients both ship tools\hdc.exe.
    $installRoots = @(
        foreach ($app in (Get-Process -Name scrcpy_client_flutter -ErrorAction SilentlyContinue)) {
            if ($app.Path) { Split-Path -Parent $app.Path }
        }
        if ($env:OS -eq 'Windows_NT') {
            $key = 'Microsoft\Windows\CurrentVersion\Uninstall\{B6E4D4F9-9E86-4E7F-8A1C-C6F9B0F8C3A1}_is1'
            foreach ($prefix in 'HKCU:\Software', 'HKLM:\Software', 'HKLM:\Software\WOW6432Node') {
                (Get-ItemProperty -LiteralPath "$prefix\$key" -ErrorAction SilentlyContinue).InstallLocation
            }
        }
        if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Programs\HongJing' }
        if ($env:ProgramW6432) { Join-Path $env:ProgramW6432 'HongJing' }
        if ($env:ProgramFiles) { Join-Path $env:ProgramFiles 'HongJing' }
    )
    foreach ($dir in $installRoots) {
        if ($dir) {
            $file = Join-Path $dir 'tools\hdc.exe'
            if (Test-Path -LiteralPath $file -PathType Leaf) { return $file }
        }
    }
    $onPath = Get-Command hdc.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) { return $onPath.Source }
    foreach ($dir in $env:ProgramFiles, $env:ProgramW6432) {
        if ($dir) {
            $file = Join-Path $dir 'Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
            if (Test-Path -LiteralPath $file -PathType Leaf) { return $file }
        }
    }
    if ($env:LOCALAPPDATA) {
        $sdk = Join-Path $env:LOCALAPPDATA 'Huawei\Sdk\openharmony'
        $versions = Get-ChildItem -LiteralPath $sdk -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d{1,4}$' } | Sort-Object { [int]$_.Name } -Descending
        foreach ($version in $versions) {
            $file = Join-Path $version.FullName 'toolchains\hdc.exe'
            if (Test-Path -LiteralPath $file -PathType Leaf) { return $file }
        }
    }
    throw '找不到 hdc.exe。请先打开 Windows 版鸿镜，或将环境变量 HDC 设为 hdc.exe 的完整路径。'
}

function Invoke-Hdc([string[]] $NativeArgs, [int] $Timeout = 20, [switch] $AllowFailure,
                    [string] $WorkingDirectory = $shellDirectory) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $script:hdc
    $start.WorkingDirectory = $WorkingDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    # Windows PowerShell 5.1 lacks ProcessStartInfo.ArgumentList. Apply the
    # Windows argv rules to quotes and trailing backslashes, without cmd.exe.
    $start.Arguments = ($NativeArgs | ForEach-Object {
        '"' + (($_ -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $out = $process.StandardOutput.ReadToEndAsync()
        $err = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit($Timeout * 1000)) {
            try { $process.Kill() } catch {}
            throw 'HDC 等待超时，请检查 USB 调试连接。'
        }
        if (!$out.Wait(2000) -or !$err.Wait(2000)) { throw 'HDC 输出超时，请检查设备连接。' }
        $message = (($out.Result, $err.Result) -join [Environment]::NewLine).Replace([string][char]13, '').Trim()
        if ($process.ExitCode -ne 0 -and !$AllowFailure) {
            throw "HDC 执行失败（$($process.ExitCode)）。$message"
        }
        return $message
    } finally { $process.Dispose() }
}

function Get-UsbSerials {
    $listing = Invoke-Hdc -NativeArgs @('list', 'targets', '-v') -Timeout 8
    foreach ($row in ($listing -split '\n')) {
        if ($row -match '^\s*(\S+)\s+USB\s+Connected(?:\s|$)') { $Matches[1] }
    }
}

function Invoke-Board([string] $Command, [string] $Serial = $script:target, [int] $Timeout = 20, [switch] $AllowFailure) {
    Invoke-Hdc -NativeArgs @('-t', $Serial, 'shell', $Command) -Timeout $Timeout -AllowFailure:$AllowFailure
}

function Test-K3([string] $Serial) {
    $brand = Invoke-Board 'param get const.product.brand' -Serial $Serial -Timeout 8
    if ($brand -cne 'Spacemit K3-Pico') { return $false }
    return (Invoke-Board 'uname -m' -Serial $Serial -Timeout 8) -ceq 'riscv64'
}

function Select-K3 {
    $usb = @(Get-UsbSerials)
    if ($usb.Count -eq 0) { throw '未找到已连接的 USB 设备，请连接 K3 并授权 USB 调试。' }
    $boards = @(foreach ($serial in $usb) {
        try {
            if (Test-K3 $serial) { $serial }
        } catch {
            if ($usb.Count -eq 1) { throw }
            Write-Warning "无法读取 USB 设备 $serial 的信息，已跳过这台设备。"
        }
    })
    if ($boards.Count -eq 0) { throw '已连接的 USB 设备中没有 RISC-V 版 SpacemiT K3-Pico。' }
    if ($boards.Count -eq 1) { return $boards[0] }
    Write-Host '选择要投屏的 K3'
    for ($i = 0; $i -lt $boards.Count; $i++) { Write-Host "  $($i + 1)) $($boards[$i])" }
    while ($true) {
        $choice = Read-Host '输入设备编号，输入 q 退出'
        if ($null -eq $choice -or $choice.Trim() -eq 'q') { return '' }
        $index = 0
        if ([int]::TryParse($choice, [ref]$index) -and $index -ge 1 -and $index -le $boards.Count) {
            return $boards[$index - 1]
        }
        Write-Host '请输入列表中的设备编号。'
    }
}

function Send-FileChecked([string] $Relative, [string] $Destination) {
    $sourceArg = $Relative.Replace([char]'/', [IO.Path]::DirectorySeparatorChar)
    $local = Join-Path $packageRoot $sourceArg
    if (!(Test-Path -LiteralPath $local -PathType Leaf)) { throw "启动包缺少 $Relative，请完整解压。" }
    $expected = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
    $remote = "$script:stage/$Destination"
    # The native client's working directory is the package. Its source argument
    # therefore contains only ASCII even when the extracted folder contains 中文.
    if (!$script:useAsciiTransfer) {
        try {
            $transfer = Invoke-Hdc -NativeArgs @('-t', $script:target, 'file', 'send', $sourceArg, $remote) -Timeout 60 -AllowFailure -WorkingDirectory $packageRoot
        } catch {
            if ($env:OS -ne 'Windows_NT') { throw }
            $transfer = $_.Exception.Message
        }
        $sum = Invoke-Board "openssl dgst -sha256 '$remote'" -AllowFailure
        if ($sum -match '\b([a-fA-F0-9]{64})$') {
            if ($Matches[1] -ieq $expected) { return }
            throw "$Relative 在 USB 传输后校验失败。"
        }
        if ($env:OS -ne 'Windows_NT') { throw "$Relative 上传失败。$transfer" }
    }

    # If an old Windows HDC cannot transfer from this cwd, send ASCII shell
    # arguments instead. Keep even the full shell command under 4 KiB.
    $encoded = [Convert]::ToBase64String([IO.File]::ReadAllBytes($local))
    $part = "$remote.b64"
    $ready = Invoke-Board ": > '$part' && echo K3_B64_READY"
    if ($ready -cne 'K3_B64_READY') { throw "无法在 K3 上暂存 $Relative。$ready" }
    for ($pos = 0; $pos -lt $encoded.Length; $pos += 2048) {
        $chunk = $encoded.Substring($pos, [Math]::Min(2048, $encoded.Length - $pos))
        [void](Invoke-Board "printf '%s' '$chunk' >> '$part'")
    }
    $sum = Invoke-Board "openssl base64 -d -A -in '$part' -out '$remote' && rm -f '$part' && openssl dgst -sha256 '$remote'"
    if ($sum -notmatch '\b([a-fA-F0-9]{64})$' -or $Matches[1] -ine $expected) {
        throw "$Relative 在 USB 传输后校验失败。"
    }
    $script:useAsciiTransfer = $true
}

function New-Stage {
    $path = Invoke-Board 'umask 077; mktemp -d /data/local/tmp/k3-mirror-stage.XXXXXXXX'
    if ($path -cnotmatch '^/data/local/tmp/k3-mirror-stage\.[a-zA-Z0-9]{8}$') {
        throw '无法建立板端临时传输目录。'
    }
    $script:stage = $path
    $reply = Invoke-Board "mkdir -m 700 '$path/720p' '$path/1080p' '$path/sw-720p' '$path/sw-1080p' && echo K3_DIRS_OK"
    if ($reply -cne 'K3_DIRS_OK') { throw "无法准备板端程序目录。$reply" }
    Send-FileChecked 'tools/board.sh' 'board.sh'
    foreach ($size in '720p', '1080p') {
        $bin = if ($size -eq '1080p') { 'bin/1080p' } else { 'bin' }
        Send-FileChecked "$bin/k3-sw-wire" "sw-$size/k3-sw-wire"
        foreach ($file in 'k3-virtual-primary', 'k3-hw-wire', 'k3-capture-pipe', 'k3-surface-capture') {
            Send-FileChecked "$bin/$file" "$size/$file"
        }
    }
}

function Remove-Stage {
    if (!$script:stage) { return }
    try {
        if (@(Get-UsbSerials) -ccontains $script:target) {
            [void](Invoke-Board "rm -rf -- '$script:stage'" -Timeout 8)
        }
    } catch { Write-Warning '设备暂时离线，未能清理本次传输的临时目录。' }
    $script:stage = ''
}

function Invoke-Switch([string] $Approved = '') {
    $cmd = "umask 077; test ! -L '$boardRoot' && mkdir -p '$boardRoot' && /bin/sh '$script:stage/board.sh' '$Resolution' '$Codec' '$script:stage' '$Approved' <'$boardRoot' && echo K3_SWITCH_OK"
    Invoke-Board $cmd -Timeout 45 -AllowFailure
}

function Show-Ready([string] $Reply) {
    foreach ($line in ($Reply -split '\n')) { if ($line -cne 'K3_SWITCH_OK') { Write-Host $line } }
    Write-Host '鸿镜显示「镜像中」时可继续使用。切换编码后若提示未连接，选择 K3 并点「连接」。'
}

function Wait-NewBoot([string] $Previous) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $reminded = $false
    while ($clock.Elapsed.TotalSeconds -lt 300) {
        try {
            if (@(Get-UsbSerials) -ccontains $script:target) {
                $current = Invoke-Board 'cat /proc/sys/kernel/random/boot_id' -Timeout 8
                if ($current -match "^$uuidPattern$" -and $current -ine $Previous -and (Test-K3 $script:target)) {
                    $display = Invoke-Board "hidumper -s DisplayManagerService -a '-s -a'" -Timeout 8
                    $window = Invoke-Board "hidumper -s WindowManagerService -a '-a'" -Timeout 8
                    if ($display.Contains('total screen num:') -and $window.Contains('--WindowManagerService--')) { return }
                }
            }
        } catch { # HDC and the graphics services can be briefly unavailable during reboot.
        }
        if (!$reminded -and $clock.Elapsed.TotalSeconds -ge 12) {
            Write-Host '正在等待同一块 K3 回到 USB 调试模式。如一直离线，请重新连接 USB 调试线。'
            $reminded = $true
        }
        Start-Sleep -Seconds 1
    }
    throw '等待开发板重新上线超时。USB 恢复后，再次运行投屏入口即可继续。'
}

try {
    if (!$Resolution) {
        $choice = Read-MenuChoice '选择投屏分辨率' '1080p' '720p'
        if ($choice -eq 'q') { exit 0 }
        $Resolution = if ($choice -eq '1') { '1080p' } else { '720p' }
    }
    if (!$Codec) {
        $choice = Read-MenuChoice '选择编码方式' '硬件编码' '软件编码'
        if ($choice -eq 'q') { exit 0 }
        $Codec = if ($choice -eq '1') { 'hw' } else { 'sw' }
    }
    $Resolution = $Resolution.ToLowerInvariant()
    $Codec = $Codec.ToLowerInvariant()
    $script:hdc = Find-Hdc $HdcPath
    $script:target = Select-K3
    if (!$script:target) { exit 0 }
    New-Stage
    $reply = Invoke-Switch
    if ($reply -cmatch '(?m)^K3_SWITCH_OK$') { Show-Ready $reply; exit 0 }
    if ($reply -cnotmatch "(?m)^K3_SWITCH_REBOOT_REQUIRED (720p|1080p) (720p|1080p) ($uuidPattern)$") {
        throw "启动未完成。$reply"
    }
    $old, $new, $boot = $Matches[1], $Matches[2], $Matches[3]
    if ($old -ceq $new -or $new -cne $Resolution) { throw '板端报告的分辨率与所选配置不一致。' }
    Write-Host "K3 当前运行 $old 主屏，切换到 $new 需要重启一次。"
    if (!$RebootIfNeeded) {
        $answer = Read-Host '重启会中断板上正在运行的程序。保存工作后，输入 Y 确认现在重启'
        if ($answer -notin 'y', 'Y') { throw '已取消切换。' }
    }
    Write-Host '正在提交重启请求，USB 重新连接后会自动继续。'
    try { $reply = Invoke-Switch $boot } catch { $reply = $_.Exception.Message }
    if ($reply -cmatch '(?m)^K3_SWITCH_OK$') { Show-Ready $reply; exit 0 }
    if ($reply -cnotmatch '(?m)^K3_SWITCH_REBOOT_SENT$') {
        $observed = ''
        try { $observed = Invoke-Board 'cat /proc/sys/kernel/random/boot_id' -Timeout 8 } catch {}
        if ($observed -ieq $boot) { throw "重启未执行。$reply" }
    }
    Wait-NewBoot $boot
    Remove-Stage
    New-Stage
    $reply = Invoke-Switch
    if ($reply -cnotmatch '(?m)^K3_SWITCH_OK$') { throw "开发板已重启，投屏未能启动。$reply" }
    Show-Ready $reply
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally { Remove-Stage }
