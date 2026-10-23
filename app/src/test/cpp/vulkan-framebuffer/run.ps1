param([string]$Cxx = 'C:/msys64/ucrt64/bin/g++.exe')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
$source = [IO.File]::ReadAllText((Join-Path $workspace 'core/Common/GPU/Vulkan/VulkanFramebuffer.cpp'))
$end = $source.IndexOf('static VkAttachmentLoadOp ConvertLoadAction')
if ($end -lt 0) { throw 'Cannot find framebuffer/render-pass implementation boundary.' }
$outputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('emucorea-framebuffer-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
$oldPath = $env:PATH
try {
    $implementation = Join-Path $outputDirectory 'framebuffer.cpp'
    [IO.File]::WriteAllText($implementation, $source.Substring(0, $end))
    $thinSource = [IO.File]::ReadAllText((Join-Path $workspace 'core/Common/GPU/Vulkan/thin3d_vulkan.cpp'))
    $factory = [regex]::Match($thinSource, '(?ms)^Framebuffer \*VKContext::CreateFramebuffer\(const FramebufferDesc &desc\) \{.*?^\}')
    if (!$factory.Success) { throw 'Cannot find production framebuffer factory.' }
    [IO.File]::WriteAllText((Join-Path $outputDirectory 'framebuffer-factory.inc'), $factory.Value)
    $managerSource = [IO.File]::ReadAllText((Join-Path $workspace 'core/GPU/Common/FramebufferManagerCommon.cpp'))
    $allocation = $managerSource.IndexOf("`tvfb->fbo = draw_->CreateFramebuffer({ vfb->renderWidth, vfb->renderHeight, 1, GetFramebufferLayers(), vfb->multisampled ? msaaLevel_ : 0, true, tag });")
    $allocationEnd = $managerSource.IndexOf("`tif (Memory::IsVRAMAddress", $allocation)
    if ($allocation -lt 0 -or $allocationEnd -lt 0) { throw 'Cannot find production framebuffer resize allocation.' }
    [IO.File]::WriteAllText((Join-Path $outputDirectory 'resize-allocation.inc'), $managerSource.Substring($allocation, $allocationEnd - $allocation))
    $promotion = [regex]::Match($managerSource, '(?ms)^\t\t\tif \(\(vfb->usageFlags & FB_USAGE_COLOR_MIXED_DEPTH\).*?^\t\t\t\}')
    if (!$promotion.Success) { throw 'Cannot find native-resolution upgrade allocation.' }
    [IO.File]::WriteAllText((Join-Path $outputDirectory 'resolution-upgrade.inc'), $promotion.Value)
    $env:PATH = (Split-Path -Parent $Cxx) + [IO.Path]::PathSeparator + $env:PATH
    $binary = Join-Path $outputDirectory 'allocation-test.exe'
    & $Cxx '-std=c++17' '-Wall' '-Wextra' '-Wno-unused-parameter' '-Wno-unused-variable' '-Wno-reorder' '-Wno-missing-field-initializers' '-I' $outputDirectory '-I' (Join-Path $PSScriptRoot 'stubs') '-I' (Join-Path $workspace 'core') $implementation (Join-Path $PSScriptRoot 'allocation-test.cpp') '-o' $binary
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw 'Framebuffer allocation regression failed.' }
} finally {
    $env:PATH = $oldPath
    foreach ($name in @('framebuffer.cpp', 'framebuffer-factory.inc', 'resize-allocation.inc', 'resolution-upgrade.inc', 'allocation-test.exe')) {
        Remove-Item -LiteralPath (Join-Path $outputDirectory $name) -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $outputDirectory -Force
}
