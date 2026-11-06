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
# Rift Apart VR installer. Copies the mod's two files and its ofxr folder (frame generation) into the game's folder,
# or takes them out again.
# Double-click and type 1 to install or 2 to uninstall. From a command prompt: uninstall, or the game folder in quotes.
# To choose the folder yourself, run it with the game's folder in double quotes.
$ErrorActionPreference = 'Stop'
$here = Split-Path -LiteralPath $env:RAVR_BAT
# The mod's files sit in a files folder beside this bat in the release zip.
$filesDir = Join-Path $here 'files'
if (-not (Test-Path -LiteralPath $filesDir)) { $filesDir = $here }
$files = 'winhttp.dll', 'openxr_loader.dll'
# Frame generation (OFXR-BRIDGE, GPL-3.0, see ofxr\SOURCE.txt). Copied as a folder, removed as a folder.
$ofxrFolder = 'ofxr'
$vcUrl = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'

function Test-VcRuntime {
    $key = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue
    if ($key -and $key.Installed -eq 1) { return $true }
    return (Test-Path -LiteralPath (Join-Path $env:WINDIR 'System32\vcruntime140_1.dll'))
}
$appId = '1895880'
$uninstall = $env:RAVR_A1 -ieq 'uninstall'
$given = if ($uninstall) { $env:RAVR_A2 } else { $env:RAVR_A1 }
if (-not $env:RAVR_A1) {
    Write-Host 'Rift Apart VR'
    Write-Host ''
    Write-Host '  1  Install or update'
    Write-Host '  2  Uninstall'
    Write-Host ''
    $choice = Read-Host 'Type 1 or 2 and press Enter'
    if ($choice -eq '2') { $uninstall = $true } elseif ($choice -ne '1') { Write-Host 'Nothing done.'; exit 0 }
}

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
        $ofxrTarget = Join-Path $game $ofxrFolder
        if ((Test-Path -LiteralPath $marker) -and (Test-Path -LiteralPath $ofxrTarget)) {
            Remove-Item -LiteralPath $ofxrTarget -Recurse -Force
            Write-Host "Removed the $ofxrFolder folder"
        }
        if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
        Write-Host ''
        Write-Host 'Removed. Your settings file RiftApartVR.ini was left in place. Delete it if you want.'
        exit 0
    }
    foreach ($file in $files) {
        if (-not (Test-Path -LiteralPath (Join-Path $filesDir $file))) {
            Write-Host "$file is missing. Keep this bat next to the files folder from the zip."
            exit 4
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $filesDir $ofxrFolder))) {
        Write-Host "The $ofxrFolder folder is missing from the files folder. Unzip the whole release again."
        exit 4
    }
    foreach ($file in $files) {
        $target = Join-Path $game $file
        # Keep a dll from something else, once. Ours is known by the marker file.
        if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath $marker)) {
            Copy-Item -LiteralPath $target "$target.before-vr" -Force
            Write-Host "Kept the earlier $file as $file.before-vr"
        }
        Copy-Item -LiteralPath (Join-Path $filesDir $file) $target -Force
        $a = (Get-FileHash -LiteralPath (Join-Path $filesDir $file) -Algorithm SHA256).Hash
        $b = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($a -ne $b) { Write-Host "$file did not copy correctly."; exit 5 }
        Write-Host "Installed $file"
    }
    # The ofxr folder: copied whole, every file checked. Settings the player changed in it (ofxr\settings) are kept.
    $source = Join-Path $filesDir $ofxrFolder
    foreach ($item in Get-ChildItem -LiteralPath $source -Recurse -File) {
        $relative = $item.FullName.Substring($source.Length).TrimStart('\')
        $target = Join-Path (Join-Path $game $ofxrFolder) $relative
        if ($relative -like 'settings\*' -and (Test-Path -LiteralPath $target)) { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path -LiteralPath $target) | Out-Null
        Copy-Item -LiteralPath $item.FullName $target -Force
        if ((Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) {
            Write-Host "$ofxrFolder\$relative did not copy correctly."
            exit 5
        }
    }
    Write-Host "Installed the $ofxrFolder folder (frame generation)"
    Set-Content -LiteralPath $marker -Value 'Rift Apart VR is installed here.'
    # Frame generation's DLSS-G part needs Microsoft's Visual C++ runtime. The mod itself does not.
    if (-not (Test-VcRuntime)) {
        Write-Host ''
        Write-Host "Microsoft's Visual C++ runtime (x64) is not installed. The mod runs without it, but frame generation's DLSS-G type needs it."
        $answer = Read-Host 'Download and install it from Microsoft now? (Y/N)'
        if ($answer -match '^[Yy]') {
            $setup = Join-Path $env:TEMP 'vc_redist.x64.exe'
            try {
                Invoke-WebRequest -Uri $vcUrl -OutFile $setup -UseBasicParsing
                Start-Process -FilePath $setup -ArgumentList '/install', '/passive', '/norestart' -Wait
                if (Test-VcRuntime) { Write-Host 'Visual C++ runtime installed.' } else { Write-Host "It did not install. You can get it here: $vcUrl" }
            } catch {
                Write-Host "The download failed. You can get it here: $vcUrl"
            }
        } else {
            Write-Host "You can install it later from: $vcUrl"
        }
    }
    Write-Host ''
    Write-Host 'Done. Start your headset app and OpenXR runtime, then start the game from Steam.'
    Write-Host 'In the game, click and hold both thumbsticks for half a second for the VR menu.'
    Write-Host 'Tip: set Texture Quality to Medium or High in the game, not Ultra (see the README).'
    exit 0
} catch {
    Write-Host "Something went wrong: $_"
    exit 1
}
