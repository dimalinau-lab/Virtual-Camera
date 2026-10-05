param (
    [string]$Version = "2.4.0"
)

$ErrorActionPreference = "Stop"

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "   Publishing GitHub Releases (v$Version)" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

# 1. Retrieve GitHub Token
$credOutput = @("protocol=https", "host=github.com", "") | git credential fill
$token = ""
foreach ($line in $credOutput) {
    if ($line.StartsWith("password=")) {
        $token = $line.Substring("password=".Length).Trim()
        break
    }
}

if (-not $token) {
    Write-Error "Could not retrieve GitHub token from git credential manager"
    exit 1
}

$headers = @{
    "Authorization" = "Bearer $token"
    "User-Agent"    = "VirtualCam-ReleaseBot"
    "Accept"        = "application/vnd.github+json"
}

# Function to create or update release and upload assets
function Publish-Release {
    param (
        [string]$RepoOwner,
        [string]$RepoName,
        [string]$TagName,
        [string]$ReleaseName,
        [string]$Body,
        [string[]]$AssetPaths
    )

    Write-Host "`n--> Processing repo: $RepoOwner/$RepoName (Tag: $TagName)..." -ForegroundColor Yellow

    # Check if release already exists
    $release = $null
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoOwner/$RepoName/releases/tags/$TagName" -Headers $headers -Method Get
        Write-Host "    Found existing release ID: $($release.id)" -ForegroundColor Cyan
    } catch {
        Write-Host "    Release does not exist yet. Creating new release..." -ForegroundColor Cyan
    }

    $tempJson = [System.IO.Path]::GetTempFileName()
    try {
        if (-not $release) {
            $createPayload = @{
                tag_name         = $TagName
                target_commitish = "main"
                name             = $ReleaseName
                body             = $Body
                draft            = $false
                prerelease       = $false
            } | ConvertTo-Json
            [System.IO.File]::WriteAllText($tempJson, $createPayload, [System.Text.UTF8Encoding]::new($false))

            $relUrl = "https://api.github.com/repos/$RepoOwner/$RepoName/releases"
            $jsonRes = & curl.exe -s -S -X POST `
                -H "Authorization: Bearer $token" `
                -H "User-Agent: VirtualCam-ReleaseBot" `
                -H "Accept: application/vnd.github+json" `
                -H "Content-Type: application/json; charset=utf-8" `
                --data-binary "@$tempJson" `
                "$relUrl"
            $release = $jsonRes | ConvertFrom-Json
            Write-Host "    Release created! ID: $($release.id)" -ForegroundColor Green
        } else {
            $updatePayload = @{
                name = $ReleaseName
                body = $Body
            } | ConvertTo-Json
            [System.IO.File]::WriteAllText($tempJson, $updatePayload, [System.Text.UTF8Encoding]::new($false))

            $relUrl = "https://api.github.com/repos/$RepoOwner/$RepoName/releases/$($release.id)"
            $jsonRes = & curl.exe -s -S -X PATCH `
                -H "Authorization: Bearer $token" `
                -H "User-Agent: VirtualCam-ReleaseBot" `
                -H "Accept: application/vnd.github+json" `
                -H "Content-Type: application/json; charset=utf-8" `
                --data-binary "@$tempJson" `
                "$relUrl"
            $release = $jsonRes | ConvertFrom-Json
            Write-Host "    Release updated! ID: $($release.id)" -ForegroundColor Green
        }
    }
    finally {
        if (Test-Path $tempJson) {
            Remove-Item $tempJson -Force -ErrorAction SilentlyContinue
        }
    }

    # Upload Assets
    $uploadBaseUrl = "https://uploads.github.com/repos/$RepoOwner/$RepoName/releases/$($release.id)/assets"
    
    # Check existing assets to delete duplicates before re-upload
    $existingAssets = $release.assets
    if (-not $existingAssets) {
        $existingAssets = @()
    }

    foreach ($assetPath in $AssetPaths) {
        if (-not (Test-Path $assetPath)) {
            Write-Warning "Asset file not found: $assetPath"
            continue
        }

        $fileName = Split-Path -Leaf $assetPath
        $fileSizeMb = [Math]::Round(((Get-Item $assetPath).Length / 1MB), 2)
        Write-Host "    Uploading asset: $fileName ($fileSizeMb MB)..." -ForegroundColor Yellow

        # Delete existing asset with same name if found
        $matched = $existingAssets | Where-Object { $_.name -eq $fileName }
        if ($matched) {
            foreach ($m in $matched) {
                Write-Host "      Deleting previous asset version (ID: $($m.id))..." -ForegroundColor DarkGray
                Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoOwner/$RepoName/releases/assets/$($m.id)" `
                    -Headers $headers -Method Delete | Out-Null
            }
        }

        $uploadUrl = "https://uploads.github.com/repos/$RepoOwner/$RepoName/releases/$($release.id)/assets?name=$fileName"
        $fullPath = (Get-Item $assetPath).FullName
        $resJson = & curl.exe -s -S -X POST `
            -H "Authorization: Bearer $token" `
            -H "User-Agent: VirtualCam-ReleaseBot" `
            -H "Content-Type: application/octet-stream" `
            --data-binary "@$fullPath" `
            "$uploadUrl"

        if ($LASTEXITCODE -ne 0) {
            Write-Error "curl upload failed for $fileName with exit code $LASTEXITCODE"
        } else {
            Write-Host "      Successfully uploaded $fileName!" -ForegroundColor Green
        }
    }

    Write-Host "    Repo $RepoOwner/$RepoName completed! Release URL: $($release.html_url)" -ForegroundColor Green
}

# 2. Release for PC (Virtual-Camera)
$pcReleaseNotes = Get-Content -Path "RELEASE_NOTES_v2.4.0.md" -Raw -Encoding UTF8
$pcAssets = @(
    "installer_output\VirtualCamNative_Setup_v2.4.0.exe",
    "installer_output\VirtualCamNative_Update.zip",
    "installer_output\VirtualCam-v2.4.0.apk"
)

Publish-Release `
    -RepoOwner "dimalinau-lab" `
    -RepoName "Virtual-Camera" `
    -TagName "v2.4.0" `
    -ReleaseName "v2.4.0: AVX2 60 FPS Interpolation, Unlocked Selfie 60 FPS & Universal Aspect Ratio" `
    -Body $pcReleaseNotes `
    -AssetPaths $pcAssets

# 3. Release for Android (Virtual-Camera-Android)
$androidReleaseNotes = @"
# VirtualCam Android v2.4.0 (versionCode 240)

## 🚀 What's New in v2.4.0
- **Front Camera 60 FPS Unlocked**: Removed 30 FPS ceiling across Android service and Web UI. Full 60 FPS and high-speed AE target support for selfie camera.
- **Persistent State**: 1080p resolution and camera facing (rear/front) are reliably preserved across app restarts (`SharedPreferences`).
- **Synchronized Status**: Native C++ and Web UI accurately detect active camera sensor state on startup.
- **Zero-Allocation Pipeline**: Reduced socket latency and optimized NAL unit buffering.

## 📦 Downloads
- `VirtualCam-v2.4.0.apk` (Android 8.0+)
"@

$androidAssets = @(
    "installer_output\VirtualCam-v2.4.0.apk"
)

Publish-Release `
    -RepoOwner "dimalinau-lab" `
    -RepoName "Virtual-Camera-Android" `
    -TagName "v2.4.0" `
    -ReleaseName "v2.4.0: Unlocked Selfie 60 FPS, 1080p State Persistence & Stability" `
    -Body $androidReleaseNotes `
    -AssetPaths $androidAssets

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "   All releases published successfully!" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
