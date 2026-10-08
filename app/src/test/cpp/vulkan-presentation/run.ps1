param(
    [string]$Cxx,
    [string]$LibrashaderIncludeDirectory
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
if (!$Cxx) {
    $compiler = Get-Command g++.exe, clang++.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($compiler) { $Cxx = $compiler.Source }
    elseif (Test-Path 'C:/msys64/ucrt64/bin/g++.exe') { $Cxx = 'C:/msys64/ucrt64/bin/g++.exe' }
    else { throw 'Pass -Cxx with a host C++17 compiler path.' }
}
if (!$LibrashaderIncludeDirectory) {
    $header = Get-ChildItem -LiteralPath (Join-Path $workspace 'core-native/.cxx') -Filter librashader.h -Recurse |
        Select-Object -First 1
    if (!$header) { throw 'Configure the native project first, or pass -LibrashaderIncludeDirectory.' }
    $LibrashaderIncludeDirectory = $header.DirectoryName
}
$outputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('emucorea-vulkan-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Cxx) + [IO.Path]::PathSeparator + $env:PATH
    foreach ($withShaders in @($true, $false)) {
        $binary = Join-Path $outputDirectory ('presentation-' + $withShaders + '.exe')
        $arguments = @('-std=c++17', '-Wall', '-Wextra', '-Wno-missing-field-initializers',
            '-Wno-unused-parameter', '-DLIBRA_RUNTIME_VULKAN=1',
            '-I', (Join-Path $PSScriptRoot 'stubs'), '-I', (Join-Path $workspace 'core'),
            '-I', (Join-Path $workspace 'core-native/src/main/cpp'), '-I', $LibrashaderIncludeDirectory,
            (Join-Path $PSScriptRoot 'presentation-test.cpp'),
            (Join-Path $workspace 'core-native/src/main/cpp/native_vulkan_presentation.cpp'),
            (Join-Path $workspace 'core-native/src/main/cpp/shader_chain.cpp'), '-o', $binary)
        if ($withShaders) { $arguments += '-DEMUCOREA_HAVE_LIBRASHADER=1' }
        & $Cxx @arguments
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed (shaders=$withShaders)." }
        & $binary
        if ($LASTEXITCODE -ne 0) { throw "Contract tests failed (shaders=$withShaders)." }
    }
} finally {
    $env:PATH = $oldPath
    foreach ($withShaders in @($true, $false)) {
        Remove-Item -LiteralPath (Join-Path $outputDirectory ('presentation-' + $withShaders + '.exe')) -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $outputDirectory -Force
}
