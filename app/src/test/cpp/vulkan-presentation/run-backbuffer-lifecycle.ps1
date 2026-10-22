param([string]$Cxx = 'C:/msys64/ucrt64/bin/g++.exe')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
$source = [IO.File]::ReadAllText((Join-Path $workspace 'core/Common/GPU/Vulkan/VulkanRenderManager.cpp'))
$method = [regex]::Match($source, '(?ms)^void VulkanRenderManager::DestroyBackbuffers\(\) \{.*?^\}')
if (!$method.Success) { throw 'Cannot find production DestroyBackbuffers implementation.' }
$outputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('emucorea-backbuffer-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
$oldPath = $env:PATH
try {
    [IO.File]::WriteAllText((Join-Path $outputDirectory 'destroy-backbuffers.inc'), $method.Value)
    $env:PATH = (Split-Path -Parent $Cxx) + [IO.Path]::PathSeparator + $env:PATH
    $binary = Join-Path $outputDirectory 'backbuffer-test.exe'
    & $Cxx '-std=c++17' '-Wall' '-Wextra' '-Werror' '-I' $outputDirectory (Join-Path $PSScriptRoot 'backbuffer-lifecycle-test.cpp') '-o' $binary
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw 'Backbuffer lifecycle regression test failed.' }
} finally {
    $env:PATH = $oldPath
    foreach ($name in @('destroy-backbuffers.inc', 'backbuffer-test.exe')) {
        Remove-Item -LiteralPath (Join-Path $outputDirectory $name) -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $outputDirectory -Force
}
