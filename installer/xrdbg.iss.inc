; Inno Setup snippet for a DEBUG installer of a game that uses xrdbg. Include or copy the parts into the game's own .iss.
;
; Compile the debug installer with the debug build's folder and a version above the release it is based on, and a suffix so
; it can never be mistaken for a release:
;   ISCC /DAppVersion=1.2.3.1 /DBuildDir=..\build-debug\Release /DOutSuffix=-debug /DDebugBuild=1 mygame.iss
;
; In the game's .iss, near the top:
;   #ifndef AppVersion
;   #define AppVersion "1.2.3"
;   #endif
;   #ifndef OutSuffix
;   #define OutSuffix ""
;   #endif
;   #ifndef BuildDir
;   #define BuildDir "..\build-release\Release"
;   #endif
;   ... and in [Setup]:  OutputBaseFilename=MyGame-Setup-{#AppVersion}{#OutSuffix}
;
; Then these two blocks, set DataFolder to the same text as XrDbgConfig.data_folder and path to this kit's script:

#define DataFolder "MyGameFolderName"      ; same as XrDbgConfig.data_folder
#define GameName   "My Game"

[Files]
#ifdef DebugBuild
Source: "..\xrdbg\installer\collect_debug.ps1"; DestDir: "{app}"; Flags: ignoreversion
#endif

[Icons]
#ifdef DebugBuild
; the game shortcut itself is best renamed in a debug installer so the two can be told apart:
;   Name: "{autoprograms}\{#GameName} - Debug"; Filename: "{app}\mygame.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\{#GameName} - Collect debug files"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\collect_debug.ps1"" -DataFolder ""{#DataFolder}"" -GameName ""{#GameName}"""; WorkingDir: "{app}"
#endif
