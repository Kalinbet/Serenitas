param([switch]$Debug)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$clang = 'C:/Program Files/LLVM/bin/clang.exe'
$glslc = "$env:VULKAN_SDK/Bin/glslc.exe"

if (-not $env:VULKAN_SDK) { Write-Host 'build-client.ps1: VULKAN_SDK is not set'; exit 1 }
if (-not (Test-Path $clang)) { Write-Host "build-client.ps1: $clang not found"; exit 1 }
if (-not (Test-Path $glslc)) { Write-Host "build-client.ps1: $glslc not found"; exit 1 }

New-Item -ItemType Directory -Force "$root/build" | Out-Null

foreach ($shader in Get-ChildItem "$root/shaders/*" -File -Include *.vert, *.frag, *.comp) {
    Write-Host $shader.Name
    & $glslc --target-env=vulkan1.4 -O -I "$root/shaders" -mfmt=c $shader.FullName -o "$root/build/$($shader.Name).h"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$common = @('-std=c23', '-O2', '-DUNICODE', '-D_UNICODE', '-D_CRT_SECURE_NO_WARNINGS', '-fcolor-diagnostics')
if ($Debug) { $common += @('-g', '-DSERENITAS_DEBUG') }

Write-Host 'models.c'
& $clang @common -o "$root/build/models.exe" "$root/models.c"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$root/build/models.exe" "$root/build/models.h"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host 'client.c'
& $clang @common -o "$root/build/client.exe" "$root/client.c" -I "$env:VULKAN_SDK/Include" -L "$env:VULKAN_SDK/Lib" -lvulkan-1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host 'build/client.exe'
