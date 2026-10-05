# Package Release Script for VirtualCamNative
param (
    [string]$Version = "2.4.0"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BinDir = Join-Path $ScriptDir "bin"
$OutputDir = Join-Path $ScriptDir "installer_output"

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "   VirtualCamNative Packaging Engine v$Version" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

# 1. Verify binaries
$RequiredBinaries = @("VirtualCamNative.exe", "NativeMFVirtualCam.dll")
foreach ($bin in $RequiredBinaries) {
    $p = Join-Path $BinDir $bin
    if (-not (Test-Path $p)) {
        Write-Error "Required binary not found: $p"
    }
}

# 2. Stage files for Hot-Swap ZIP
$TempStage = Join-Path $env:TEMP ("vcn_stage_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TempStage | Out-Null

try {
    Write-Host "`n[1/2] Creating Hot-Swap Update ZIP archive..." -ForegroundColor Yellow
    
    $CoreFiles = @(
        "VirtualCamNative.exe",
        "NativeMFVirtualCam.dll",
        "WebView2Loader.dll",
        "icon.ico",
        "index.html",
        "index2.html",
        "index3.html",
        "translations.js",
        "vcam_core.js",
        "tailwind.min.js",
        "anime.min.js"
    )

    foreach ($file in $CoreFiles) {
        $src = Join-Path $BinDir $file
        if (Test-Path $src) {
            Copy-Item -Path $src -Destination $TempStage -Force
        }
    }

    $ZipPath = Join-Path $OutputDir "VirtualCamNative_Update.zip"
    if (Test-Path $ZipPath) {
        Remove-Item -Path $ZipPath -Force
    }

    $TarExe = Join-Path $env:SystemRoot "System32\tar.exe"
    if (Test-Path $TarExe) {
        Push-Location $TempStage
        & $TarExe -a -c -f $ZipPath *
        Pop-Location
    } else {
        Compress-Archive -Path "$TempStage\*" -DestinationPath $ZipPath -CompressionLevel Optimal
    }

    $zipItem = Get-Item $ZipPath
    $zipMb = [Math]::Round(($zipItem.Length / 1048576), 2)
    $zipHash = (Get-FileHash $ZipPath -Algorithm SHA256).Hash

    Write-Host "   -> Hot-Swap ZIP created: $ZipPath" -ForegroundColor Green
    Write-Host "   -> Size: $zipMb MB ($($zipItem.Length) bytes)" -ForegroundColor Green
    Write-Host "   -> SHA256: $zipHash" -ForegroundColor DarkGray
}
finally {
    if (Test-Path $TempStage) {
        Remove-Item -Path $TempStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# 3. Check for Inno Setup compiler
Write-Host "`n[2/3] Checking Inno Setup Compiler..." -ForegroundColor Yellow
$InnoCompilers = @(
    "D:\Inno Setup 6\ISCC.exe",
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)

$IsccPath = $null
foreach ($path in $InnoCompilers) {
    if (Test-Path $path) {
        $IsccPath = $path
        break
    }
}

if ($IsccPath) {
    Write-Host "   -> Building full installer via $IsccPath..." -ForegroundColor Cyan
    $IssFile = Join-Path $ScriptDir "installer.iss"
    & $IsccPath $IssFile
    $InstallerExe = Join-Path $OutputDir "VirtualCamNative_Setup_v$Version.exe"
    if (Test-Path $InstallerExe) {
        $instItem = Get-Item $InstallerExe
        $instMb = [Math]::Round(($instItem.Length / 1048576), 2)
        Write-Host "   -> Full installer created: $InstallerExe ($instMb MB)" -ForegroundColor Green
    }
} else {
    Write-Host "   -> Inno Setup (ISCC.exe) not found. Full installer skipped." -ForegroundColor DarkYellow
}

# 4. Stage Android APK
Write-Host "`n[3/3] Packaging Android APK..." -ForegroundColor Yellow
$ApkCandidates = @(
    (Join-Path $ScriptDir "android\VirtualCam-v$Version.apk"),
    (Join-Path $ScriptDir "android\VirtualCam-v2.2.0.apk"),
    (Join-Path $ScriptDir "android\app\build\outputs\apk\release\app-release.apk")
)

$TargetApk = Join-Path $OutputDir "VirtualCam-v$Version.apk"
$CopiedApk = $false
foreach ($apk in $ApkCandidates) {
    if (Test-Path $apk) {
        Copy-Item -Path $apk -Destination $TargetApk -Force
        $apkItem = Get-Item $TargetApk
        $apkMb = [Math]::Round(($apkItem.Length / 1048576), 2)
        Write-Host "   -> Android APK staged: $TargetApk ($apkMb MB)" -ForegroundColor Green
        $CopiedApk = $true
        break
    }
}

if (-not $CopiedApk) {
    Write-Host "   -> Android APK not found. APK packaging skipped." -ForegroundColor DarkYellow
}

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "   Packaging completed successfully!" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
