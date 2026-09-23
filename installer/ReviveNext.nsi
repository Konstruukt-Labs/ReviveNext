; ReviveNext NSIS installer - built by `cmake --build build --target installer`
; (makensis -DROOT=<repo root> installer/ReviveNext.nsi). Ships the built
; Release layout: ReviveNext.exe plus everything under Resources/. Per-user
; state (art, titles, registration manifest) lives under
; %LOCALAPPDATA%\ReviveNext and is intentionally left behind on uninstall.

; ROOT is passed in via -DROOT=<repo root> (see the installer target in
; CMakeLists.txt).

Name "ReviveNext"
OutFile "${ROOT}\build\ReviveNext-Setup.exe"
Icon "${ROOT}\Art\revive-next.ico"
InstallDir "$PROGRAMFILES64\ReviveNext"
InstallDirRegKey HKLM "Software\ReviveNext" "InstallDir"
RequestExecutionLevel admin

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "ReviveNext"
  SetOutPath "$INSTDIR"
  File "${ROOT}\build\Release\ReviveNext.exe"
  SetOutPath "$INSTDIR\Resources"
  File /r "${ROOT}\build\Release\Resources\*.*"

  CreateDirectory "$SMPROGRAMS\ReviveNext"
  CreateShortcut "$SMPROGRAMS\ReviveNext\ReviveNext.lnk" "$INSTDIR\ReviveNext.exe"
  CreateShortcut "$SMPROGRAMS\ReviveNext\Uninstall ReviveNext.lnk" "$INSTDIR\Uninstall.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\ReviveNext" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "DisplayName" "ReviveNext"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "DisplayIcon" '"$INSTDIR\ReviveNext.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "Publisher" "Brian Gilbert"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\ReviveNext\ReviveNext.lnk"
  Delete "$SMPROGRAMS\ReviveNext\Uninstall ReviveNext.lnk"
  RMDir "$SMPROGRAMS\ReviveNext"

  Delete "$INSTDIR\ReviveNext.exe"
  RMDir /r "$INSTDIR\Resources"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext"
  DeleteRegKey HKLM "Software\ReviveNext"

  ; %LOCALAPPDATA%\ReviveNext (art picks, titles, registration manifest,
  ; autoregister log) is deliberately kept - it is the user's data.
SectionEnd
