# Build the plugin and deploy it into the local test server.
#
# A loaded plugin DLL is locked by the server process, so it cannot be
# overwritten directly. ArkApi's hot-reload watches for a sibling file named
# "<Plugin>.dll.ArkApi": when it appears it saves the world, unloads the plugin,
# copies the new file over the real DLL, removes the marker and reloads.
# See PluginManager.cpp - that is the only supported way to update in place.

param(
    [string]$ServerRoot = "C:\ARKServer",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$project = Split-Path -Parent $PSScriptRoot

if (-not $SkipBuild) {
    Write-Host "Building..." -ForegroundColor Cyan
    cmake --build "$project\build" --config Release
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$dll = "$project\dist\StructureBlueprint\StructureBlueprint.dll"
if (-not (Test-Path $dll)) { throw "missing $dll" }

$target = "$ServerRoot\ShooterGame\Binaries\Win64\ArkApi\Plugins\StructureBlueprint"
New-Item -ItemType Directory -Force -Path $target | Out-Null

Copy-Item "$project\dist\StructureBlueprint\PluginInfo.json" $target -Force

$live = "$target\StructureBlueprint.dll"
$serverUp = [bool](Get-Process ShooterGameServer -ErrorAction SilentlyContinue)

if ($serverUp -and (Test-Path $live)) {
    # Hand it to ArkApi's reloader rather than fighting the file lock.
    Copy-Item $dll "$target\StructureBlueprint.dll.ArkApi" -Force
    Write-Host "Staged as StructureBlueprint.dll.ArkApi - ArkApi reloads within ~5s" -ForegroundColor Green
    Write-Host "Watch for 'Reloaded plugin - StructureBlueprint' in Win64\logs\" -ForegroundColor DarkGray
} else {
    # Server down (or first install): write the DLL directly.
    Copy-Item $dll $target -Force
    Write-Host "Copied directly (server not running)" -ForegroundColor Green
}

Get-ChildItem $target | Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB)}}, LastWriteTime |
    Format-Table -AutoSize
