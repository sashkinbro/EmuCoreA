param(
    [Parameter(Mandatory = $true)][string]$DeviceSerial,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$SdkRoot = "$env:LOCALAPPDATA/Android/Sdk"
)

$ErrorActionPreference = 'Stop'
$auditRoot = (Resolve-Path "$PSScriptRoot/../../../..").Path
$auditVariant = $Configuration.ToLowerInvariant()
$auditLibDir = "$auditRoot/app/build/intermediates/stripped_native_libs/$auditVariant/strip${Configuration}DebugSymbols/out/lib/arm64-v8a"
$auditOutput = "$auditRoot/out/native-device-tests/$auditVariant"
$auditClang = "$SdkRoot/ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
$auditAdb = "$SdkRoot/platform-tools/adb.exe"
$auditRemote = '/data/local/tmp/emucorea-native-tests'

function Invoke-AuditAdb([string[]]$CommandArgs) {
    & $auditAdb -s $DeviceSerial @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $($CommandArgs[0]) (exit $LASTEXITCODE)" }
}

if (!(Test-Path "$auditLibDir/libemucorea_core.so")) {
    throw "Build :app:assemble$Configuration before running the native probes."
}
New-Item -ItemType Directory -Force $auditOutput | Out-Null
$auditTests = @{
    module_handler_lifecycle_test = @("$PSScriptRoot/module_handler_lifecycle_test.cpp")
    stereo_resampler_test = @("$PSScriptRoot/stereo_resampler_test.cpp")
    upstream_math_test = @("$PSScriptRoot/upstream_math_test.cpp", "$auditRoot/core/unittest/TestGEMath.cpp", "$auditRoot/core/unittest/TestSplineTessellation.cpp")
}
foreach ($auditTest in $auditTests.Keys) {
    & $auditClang '--target=aarch64-linux-android26' '-std=c++17' '-O2' "-I$auditRoot/core" "-I$auditRoot/core/ext" @($auditTests[$auditTest]) "-L$auditLibDir" '-lemucorea_core' '-o' "$auditOutput/$auditTest"
    if ($LASTEXITCODE -ne 0) { throw "Native probe compilation failed: $auditTest" }
}

Invoke-AuditAdb -CommandArgs @('shell', 'mkdir', '-p', $auditRemote)
try {
    foreach ($auditLibrary in @('libemucorea_core.so', 'libc++_shared.so', 'liblibrashader_capi.so')) {
        if (Test-Path "$auditLibDir/$auditLibrary") {
            Invoke-AuditAdb -CommandArgs @('push', "$auditLibDir/$auditLibrary", "$auditRemote/")
        }
    }
    foreach ($auditTest in $auditTests.Keys) {
        Invoke-AuditAdb -CommandArgs @('push', "$auditOutput/$auditTest", "$auditRemote/")
        Invoke-AuditAdb -CommandArgs @('shell', "cd $auditRemote && chmod 755 $auditTest && LD_LIBRARY_PATH=. ./$auditTest")
    }
} finally {
    $auditResolvedRemote = & $auditAdb -s $DeviceSerial shell "readlink -f $auditRemote"
    if ($LASTEXITCODE -eq 0 -and $auditResolvedRemote.Trim() -eq $auditRemote) {
        Invoke-AuditAdb -CommandArgs @('shell', 'rm', '-rf', $auditRemote)
    }
}
