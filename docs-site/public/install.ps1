#
# Mystral Native.js Installer for Windows
# https://mystralengine.github.io/mystralnative/
#
# Usage: irm https://mystralengine.github.io/mystralnative/install.ps1 | iex
#

$ErrorActionPreference = "Stop"

$Repo = "mystralengine/mystralnative"
$InstallDir = if ($env:MYSTRAL_INSTALL_DIR) { $env:MYSTRAL_INSTALL_DIR } else { "$HOME\.mystral" }

function Write-Banner {
    Write-Host ""
    Write-Host "  __  __           _             _ " -ForegroundColor Blue
    Write-Host " |  \/  |_   _ ___| |_ _ __ __ _| |" -ForegroundColor Blue
    Write-Host " | |\/| | | | / __| __| '__/ `` | |" -ForegroundColor Blue
    Write-Host " | |  | | |_| \__ \ |_| | | (_| | |" -ForegroundColor Blue
    Write-Host " |_|  |_|\__, |___/\__|_|  \__,_|_|" -ForegroundColor Blue
    Write-Host "         |___/              Native" -ForegroundColor Blue
    Write-Host ""
}

function Write-Info($msg) {
    Write-Host "==> " -NoNewline -ForegroundColor Blue
    Write-Host $msg
}

function Write-Success($msg) {
    Write-Host "==> " -NoNewline -ForegroundColor Green
    Write-Host $msg
}

function Write-Warn($msg) {
    Write-Host "Warning: " -NoNewline -ForegroundColor Yellow
    Write-Host $msg
}

function Write-Error-Exit($msg) {
    Write-Host "Error: " -NoNewline -ForegroundColor Red
    Write-Host $msg
    exit 1
}

function Get-Platform {
    $arch = $env:PROCESSOR_ARCHITECTURE
    switch ($arch) {
        "AMD64" { return "windows-x64" }
        "ARM64" { return "windows-arm64" }
        "x86"   {
            # 32-bit PowerShell on 64-bit OS reports x86; check the real OS arch
            $osArch = (Get-CimInstance Win32_OperatingSystem).OSArchitecture
            if ($osArch -like "64*" -or $osArch -like "*x64*") { return "windows-x64" }
            Write-Error-Exit "32-bit Windows is not supported."
        }
        default { Write-Error-Exit "Unsupported architecture: $arch" }
    }
}

function Get-LatestVersion {
    Write-Info "Fetching latest release..."
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ "User-Agent" = "MystralInstaller" }
        $version = $release.tag_name
        if (-not $version) {
            Write-Error-Exit "Failed to fetch latest version."
        }
        Write-Info "Latest version: $version"
        return $version
    } catch {
        Write-Error-Exit "Failed to fetch latest version. Check your internet connection. $_"
    }
}

function Install-Mystral {
    Write-Banner

    $platform = Get-Platform
    Write-Info "Detected platform: $platform"

    $version = Get-LatestVersion

    # Default to V8 + Dawn
    $buildName = "mystral-$platform-v8-dawn"
    $downloadUrl = "https://github.com/$Repo/releases/download/$version/$buildName.zip"
    Write-Info "Selected build: $buildName"

    # Download
    $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "mystral-install-$(Get-Random)"
    New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
    $zipFile = Join-Path $tmpDir "mystral.zip"

    Write-Info "Downloading $buildName..."
    try {
        Invoke-WebRequest -Uri $downloadUrl -OutFile $zipFile -UseBasicParsing
    } catch {
        Write-Error-Exit "Download failed. Check if the release exists: $downloadUrl"
    }

    # Extract
    Write-Info "Extracting to $InstallDir..."
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    Expand-Archive -Path $zipFile -DestinationPath $tmpDir -Force

    # Remove zip before moving files
    Remove-Item -Path $zipFile -Force

    # Move files — handle both nested ($buildName/ subdir) and flat (files at root) zip structures
    $nestedDir = Join-Path $tmpDir $buildName
    if (Test-Path $nestedDir) {
        Get-ChildItem -Path $nestedDir | Move-Item -Destination $InstallDir -Force
    } else {
        Get-ChildItem -Path $tmpDir | Move-Item -Destination $InstallDir -Force
    }

    # Cleanup
    Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

    Write-Success "Installed to $InstallDir"

    # Add to PATH
    $currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($currentPath -notlike "*$InstallDir*") {
        [Environment]::SetEnvironmentVariable("Path", "$currentPath;$InstallDir", "User")
        Write-Info "Added $InstallDir to user PATH"
        # Also update current session
        $env:Path = "$env:Path;$InstallDir"
    } else {
        Write-Info "PATH already configured"
    }

    # Verify
    $mystralExe = Join-Path $InstallDir "mystral.exe"
    if (Test-Path $mystralExe) {
        Write-Success "Installation complete!"
        Write-Host ""
        Write-Host "  The PATH has been updated. Restart your terminal, then run:" -ForegroundColor White
        Write-Host ""
        Write-Host "    mystral --version" -ForegroundColor Blue
        Write-Host ""
        Write-Host "  Run an example:" -ForegroundColor White
        Write-Host ""
        Write-Host "    cd $InstallDir" -ForegroundColor Blue
        Write-Host "    mystral run examples\triangle.js" -ForegroundColor Blue
        Write-Host ""
        Write-Host "  Documentation: https://mystralengine.github.io/mystralnative/" -ForegroundColor Blue
        Write-Host ""
    } else {
        Write-Error-Exit "Installation failed. mystral.exe not found."
    }
}

Install-Mystral
