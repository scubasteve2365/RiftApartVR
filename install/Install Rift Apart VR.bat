<# : batch part. This file is both a bat file and a PowerShell script.
@echo off
setlocal
set "RAVR_BAT=%~f0"
set "RAVR_A1=%~1"
set "RAVR_A2=%~2"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-Expression ([IO.File]::ReadAllText($env:RAVR_BAT))"
set "RAVR_CODE=%ERRORLEVEL%"
if not defined RAVR_NOPAUSE pause
exit /b %RAVR_CODE%
#>
# Rift Apart VR installer. Copies two files into the game's folder, or takes them out again.
# Double-click to install. To remove the mod, run it from a command prompt with: uninstall
# To choose the folder yourself, run it with the game's folder in double quotes.
$ErrorActionPreference = 'Stop'
$here = Split-Path -LiteralPath $env:RAVR_BAT
$files = 'winhttp.dll', 'openxr_loader.dll'
$appId = '1895880'
$uninstall = $env:RAVR_A1 -ieq 'uninstall'
$given = if ($uninstall) { $env:RAVR_A2 } else { $env:RAVR_A1 }

function Find-Game {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        $item = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($item) {
            if ($item.SteamPath) { $steam = $item.SteamPath; break }
            if ($item.InstallPath) { $steam = $item.InstallPath; break }
        }
    }
    if (-not $steam) { return $null }
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($match in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"')) {
            $libraries += $match.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($library in $libraries) {
        $manifest = Join-Path $library "steamapps\appmanifest_$appId.acf"
        if (Test-Path -LiteralPath $manifest) {
            $name = [regex]::Match([IO.File]::ReadAllText($manifest), '"installdir"\s+"([^"]+)"').Groups[1].Value
            $folder = Join-Path $library "steamapps\common\$name"
            if (Test-Path -LiteralPath (Join-Path $folder 'RiftApart.exe')) { return $folder }
        }
    }
    return $null
}

try {
    Write-Host 'Rift Apart VR'
    Write-Host ''
    $game = if ($given) { $given } else { Find-Game }
    if (-not $game -or -not (Test-Path -LiteralPath (Join-Path $game 'RiftApart.exe'))) {
        Write-Host 'Could not find Ratchet & Clank: Rift Apart.'
        Write-Host 'Run this from a command prompt with the game folder in double quotes after its name.'
        exit 2
    }
    Write-Host "Game folder: $game"
    if (Get-Process -Name 'RiftApart' -ErrorAction SilentlyContinue) {
        Write-Host 'The game is running. Close it and try again.'
        exit 3
    }
    # The game's helper programs also load winhttp.dll and can stay open after the game.
    Get-Process -Name 'crs-handler', 'crs-video' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    $marker = Join-Path $game 'RiftApartVR.installed'
    if ($uninstall) {
        foreach ($file in $files) {
            $target = Join-Path $game $file
            if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force; Write-Host "Removed $file" }
            $saved = "$target.before-vr"
            if (Test-Path -LiteralPath $saved) { Move-Item -LiteralPath $saved $target -Force; Write-Host "Put back the earlier $file" }
        }
        if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
        Write-Host ''
        Write-Host 'Removed. Your settings file RiftApartVR.ini was left in place. Delete it if you want.'
        exit 0
    }
    foreach ($file in $files) {
        if (-not (Test-Path -LiteralPath (Join-Path $here $file))) {
            Write-Host "$file is missing. Keep this file next to the two dll files."
            exit 4
        }
    }
    foreach ($file in $files) {
        $target = Join-Path $game $file
        # Keep a dll from something else, once. Ours is known by the marker file.
        if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath $marker)) {
            Copy-Item -LiteralPath $target "$target.before-vr" -Force
            Write-Host "Kept the earlier $file as $file.before-vr"
        }
        Copy-Item -LiteralPath (Join-Path $here $file) $target -Force
        $a = (Get-FileHash -LiteralPath (Join-Path $here $file) -Algorithm SHA256).Hash
        $b = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($a -ne $b) { Write-Host "$file did not copy correctly."; exit 5 }
        Write-Host "Installed $file"
    }
    Set-Content -LiteralPath $marker -Value 'Rift Apart VR is installed here.'
    Write-Host ''
    Write-Host 'Done. Start your headset app and OpenXR runtime, then start the game from Steam.'
    Write-Host 'In the game, click and hold both thumbsticks for half a second for the VR menu.'
    exit 0
} catch {
    Write-Host "Something went wrong: $_"
    exit 1
}
