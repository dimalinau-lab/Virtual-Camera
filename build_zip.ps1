$ErrorActionPreference = "Stop"
$stageDir = "installer_output\stage_zip"
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

# Copy binaries and assets
Copy-Item "bin\*" $stageDir -Recurse -Force
# Copy updated APK
if (Test-Path "installer_output\app-debug.apk") {
    Copy-Item "installer_output\app-debug.apk" (Join-Path $stageDir "VirtualCam-v2.4.6.apk") -Force
}

$zipPath = "installer_output\VirtualCamNative_v2.4.6_Full.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -Force
Remove-Item -Recurse -Force $stageDir

Get-Item $zipPath | Select-Object Name, Length, LastWriteTime

