; Inno Setup 6 Script for VirtualCamNative
; Ultra-low-latency C++20 DirectShow & MediaFoundation Virtual Camera

#define MyAppName "VirtualCamNative"
#define MyAppVersion "2.2.0"
#define MyAppPublisher "dimalinau-lab"
#define MyAppURL "https://github.com/dimalinau-lab/Virtual-Camera"
#define MyAppExeName "VirtualCamNative.exe"

[Setup]
AppId={{E5D4B2A1-8899-4A7B-91E2-F4A3C8B71201}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=installer_output
OutputBaseFilename=VirtualCamNative_Setup_v2.2.0
SetupIconFile=bin\icon.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main Executable & Virtual Camera COM Driver
Source: "bin\VirtualCamNative.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\NativeMFVirtualCam.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\icon.ico"; DestDir: "{app}"; Flags: ignoreversion

; Android ADB Bridge
Source: "bin\adb.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\AdbWinApi.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\AdbWinUsbApi.dll"; DestDir: "{app}"; Flags: ignoreversion

; FFmpeg Shared Libraries
Source: "bin\avcodec-63.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\avdevice-63.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\avfilter-12.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\avformat-63.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\avutil-61.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\swresample-7.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\swscale-10.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\ffmpeg.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\ffplay.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\ffprobe.exe"; DestDir: "{app}"; Flags: ignoreversion

; VC++ Runtime Redistributable
Source: "bin\VC_redist.x64.exe"; DestDir: "{app}"; Flags: ignoreversion

; UI Skins & Defaults (preserves existing config on upgrade)
Source: "bin\index.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\index2.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\index3.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\config.json"; DestDir: "{app}"; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon

[Run]
; Silent installation of Microsoft Visual C++ runtime if needed
Filename: "{app}\VC_redist.x64.exe"; Parameters: "/passive /norestart"; Flags: runhidden; StatusMsg: "Configuring Microsoft Visual C++ 2015-2022 Redistributable..."
; Silent registration of 64-bit DirectShow & MediaFoundation Virtual Camera COM filter
Filename: "regsvr32.exe"; Parameters: "/s ""{app}\NativeMFVirtualCam.dll"""; StatusMsg: "Registering DirectShow Virtual Camera Filter..."
; Launch VirtualCamNative
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Silent unregistration of COM Virtual Camera filter
Filename: "regsvr32.exe"; Parameters: "/u /s ""{app}\NativeMFVirtualCam.dll"""; Flags: runhidden
