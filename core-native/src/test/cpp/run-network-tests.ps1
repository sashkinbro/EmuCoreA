param(
    [Parameter(Mandatory = $true)][string]$DeviceSerial,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$CompileOnly,
    [string]$SdkRoot = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
$networkRoot = (Resolve-Path "$PSScriptRoot/../../../..").Path
$networkVariant = $Configuration.ToLowerInvariant()
$networkLibDir = "$networkRoot/app/build/intermediates/stripped_native_libs/$networkVariant/strip${Configuration}DebugSymbols/out/lib/arm64-v8a"
$networkOutput = "$networkRoot/out/network-device-tests/$networkVariant"
$networkClang = "$SdkRoot/ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
$networkAdb = "$SdkRoot/platform-tools/adb.exe"
# A fresh directory prevents stale barriers from satisfying discovery/reboot assertions.
$networkRemote = '/data/local/tmp/emucorea-network-' + [guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Force $networkOutput | Out-Null
& $networkClang '--target=aarch64-linux-android26' '-std=c++17' '-O2' "-I$networkRoot/core" "-I$networkRoot/core/ext" `
    "$PSScriptRoot/network_multiplayer_test.cpp" "-L$networkLibDir" '-lemucorea_core' '-o' "$networkOutput/network_multiplayer_test"
if ($LASTEXITCODE -ne 0) { throw 'Network probe compilation failed' }
if ($CompileOnly) { Write-Output "Compiled $networkOutput/network_multiplayer_test (device transport not executed)"; return }
function Invoke-NetworkAdb([string[]]$CommandArgs) {
    & $networkAdb -s $DeviceSerial @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $($CommandArgs[0])" }
}
Invoke-NetworkAdb -CommandArgs @('shell', 'mkdir', '-p', $networkRemote)
foreach ($networkLibrary in @('libemucorea_core.so', 'libc++_shared.so', 'liblibrashader_capi.so')) {
    if (Test-Path "$networkLibDir/$networkLibrary") { Invoke-NetworkAdb -CommandArgs @('push', "$networkLibDir/$networkLibrary", "$networkRemote/") }
}
Invoke-NetworkAdb -CommandArgs @('push', "$networkOutput/network_multiplayer_test", "$networkRemote/")
Invoke-NetworkAdb -CommandArgs @('shell', 'chmod', '755', "$networkRemote/network_multiplayer_test")
$networkOffsets = @(10000, 0)
try {
    foreach ($networkOffset in $networkOffsets) {
        $networkWork = "$networkRemote/offset$networkOffset"
        Invoke-NetworkAdb -CommandArgs @('shell', 'mkdir', '-p', $networkWork)
        foreach ($networkRole in @('server', 'client')) {
            # exec makes the PID recorded by the native probe belong to the
            # executable. Closing all three standard FDs releases adb's channel.
            $networkLaunch = "nohup sh -c 'exec env LD_LIBRARY_PATH=$networkRemote $networkRemote/network_multiplayer_test $networkRole $networkWork $networkOffset' </dev/null >$networkWork/$networkRole.log 2>&1 &"
            Invoke-NetworkAdb -CommandArgs @('shell', $networkLaunch)
        }
        $networkDeadline = [DateTime]::UtcNow.AddSeconds(210)
        $networkComplete = $false
        do {
            $networkState = & $networkAdb -s $DeviceSerial shell "if [ -f $networkWork/server.exit-0 ] && [ -f $networkWork/client.exit-0 ]; then echo complete; else for marker in $networkWork/*.exit-*; do [ -f `"`$marker`" ] || continue; case `"`$marker`" in *.exit-0) ;; *) echo failed; exit 0;; esac; done; echo running; fi"
            if ($LASTEXITCODE -ne 0) { throw 'Unable to poll network probes' }
            if (($networkState -join '').Trim() -eq 'complete') { $networkComplete = $true; break }
            if (($networkState -join '').Trim() -eq 'failed') { throw "Network peer failed at port offset $networkOffset" }
            Start-Sleep -Milliseconds 500
        } while ([DateTime]::UtcNow -lt $networkDeadline)
        if (!$networkComplete) { throw "Network transport/teardown timed out at port offset $networkOffset" }
        Write-Output "Both cores completed two sessions at port offset $networkOffset"
    }
} finally {
    foreach ($networkOffset in $networkOffsets) {
        $networkWork = "$networkRemote/offset$networkOffset"
        # Only kill PIDs recorded in this run, after checking their current argv[0]
        # against our unique absolute executable path (no global process matching).
        $networkStop = @'
for role in server client; do
  file=WORK/$role.pid
  [ -f "$file" ] || continue
  read peer < "$file"
  case "$peer" in ''|*[!0-9]*) continue;; esac
  [ -r /proc/$peer/cmdline ] || continue
  first=$(tr '\000' '\n' < /proc/$peer/cmdline | head -n 1)
  [ "$first" = 'BINARY' ] || continue
  kill "$peer" 2>/dev/null || true
  sleep 1
  [ -r /proc/$peer/cmdline ] || continue
  first=$(tr '\000' '\n' < /proc/$peer/cmdline | head -n 1)
  [ "$first" = 'BINARY' ] && kill -9 "$peer" 2>/dev/null || true
done
'@
        $networkStop = $networkStop.Replace('WORK', $networkWork).Replace('BINARY', "$networkRemote/network_multiplayer_test").Replace("`r", '')
        & $networkAdb -s $DeviceSerial shell $networkStop
        foreach ($networkRole in @('server', 'client')) {
            & $networkAdb -s $DeviceSerial pull "$networkWork/$networkRole.log" "$networkOutput/offset$networkOffset-$networkRole.log"
        }
    }
    # Resolve and verify the exact directory before recursive removal. Paths are
    # generated locally; no device-provided path is ever interpolated for removal.
    $networkResolved = & $networkAdb -s $DeviceSerial shell readlink -f $networkRemote
    if ($LASTEXITCODE -eq 0 -and ($networkResolved -join '').Trim() -eq $networkRemote -and
        $networkRemote -match '^/data/local/tmp/emucorea-network-[0-9a-f]{32}$') {
        Invoke-NetworkAdb -CommandArgs @('shell', 'rm', '-rf', $networkRemote)
    } else { Write-Warning "Remote cleanup skipped: exact path could not be verified ($networkRemote)" }
    Write-Output "Device logs saved in $networkOutput"
}
foreach ($networkOffset in $networkOffsets) {
    foreach ($networkRole in @('server', 'client')) {
        $networkLog = Get-Content "$networkOutput/offset$networkOffset-$networkRole.log" -Raw
        foreach ($networkRound in @(1, 2)) {
            if ($networkLog -notmatch "$networkRole round=$networkRound discovery/PDP/PTP nonce\+MAC\+port PASS") {
                throw "Missing transport PASS for $networkRole round $networkRound offset $networkOffset"
            }
        }
    }
}
Write-Output 'Two real Android PPSSPP cores: discovery + PDP UDP + PTP TCP + second session, offsets 10000 and 0 PASS'
