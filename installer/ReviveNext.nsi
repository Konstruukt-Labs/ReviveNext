; ReviveNext NSIS installer - built by `cmake --build build --target installer`
; (makensis -DROOT=<repo root> installer/ReviveNext.nsi). Ships the built
; Release layout: ReviveNext.exe plus everything under Resources/. Per-user
; state (art, titles, registration manifest) lives under
; %LOCALAPPDATA%\ReviveNext and is intentionally left behind on uninstall.

; ROOT is passed in via -DROOT=<repo root> (see the installer target in
; CMakeLists.txt), along with the CalVer build stamp: VERSION in
; yyyy.mm.dd form (installer title, DisplayVersion, ProductVersion
; strings) and VERSION4 as an unpadded four-part numeric form for
; VIProductVersion, whose fields don't accept zero-padding.

!include "nsDialogs.nsh"
!include "LogicLib.nsh"

Name "ReviveNext ${VERSION}"
OutFile "${ROOT}\build\ReviveNext-Setup.exe"
Icon "${ROOT}\Art\revive-next.ico"
InstallDir "$PROGRAMFILES64\ReviveNext"
InstallDirRegKey HKLM "Software\ReviveNext" "InstallDir"
RequestExecutionLevel admin
VIProductVersion "${VERSION4}"
VIAddVersionKey "ProductName" "ReviveNext"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "ReviveNext Setup"
VIAddVersionKey "LegalCopyright" "Brian Gilbert"

Page directory
Page custom SupportPageCreate SupportPageLeave
Page instfiles
Page custom DonePageCreate DonePageLeave
UninstPage uninstConfirm
UninstPage instfiles

; The installer often starts behind other windows (launched from an
; elevated/UAC transition), so pull it to the foreground explicitly
; once the dialog exists.
Function .onGUIInit
	BringToFront
	System::Call "user32::SetForegroundWindow(i $HWNDPARENT)"
FunctionEnd

Var LinkCoffee
Var LinkSponsor

Function OnCoffeeLink
	ExecShell "open" "https://buymeacoffee.com/itsvrk"
FunctionEnd

Function OnSponsorLink
	ExecShell "open" "https://github.com/sponsors/BrianGilbert"
FunctionEnd

Var LaunchCheckbox
Var ShortcutStartMenu
Var ShortcutDesktop

Function DonePageLeave
	${If} $ShortcutStartMenu == ${BST_CHECKED}
		CreateDirectory "$SMPROGRAMS\ReviveNext"
		CreateShortcut "$SMPROGRAMS\ReviveNext\ReviveNext.lnk" "$INSTDIR\ReviveNext.exe"
		CreateShortcut "$SMPROGRAMS\ReviveNext\Uninstall ReviveNext.lnk" "$INSTDIR\Uninstall.exe"
	${EndIf}
	${If} $ShortcutDesktop == ${BST_CHECKED}
		CreateShortcut "$DESKTOP\ReviveNext.lnk" "$INSTDIR\ReviveNext.exe"
	${EndIf}
	${If} $LaunchCheckbox == ${BST_CHECKED}
		Exec "$INSTDIR\ReviveNext.exe"
	${EndIf}
FunctionEnd

Function DonePageCreate
	nsDialogs::Create 1018
	Pop $0
	${NSD_CreateLabel} 0 0 100% 150u "Making your games show up in SteamVR's Library (Other Apps tab):$\n$\n1. Have SteamVR running (start it if it is not).$\n2. Launch ReviveNext - the art review window opens; close it whenever you are done. This registers your games and arms the automatic refresh.$\n3. The games appear under Other Apps. If the list does not refresh right away, restart SteamVR once.$\n$\nFrom then on it is fully automatic: every SteamVR start refreshes the list, and newly installed games appear on their own.$\n$\n(Cover art matching is optional - enter a free SteamGridDB API key via the Settings button in the application window.)"
	Pop $0
	nsDialogs::Show
FunctionEnd

Function SupportPageCreate
	nsDialogs::Create 1018
	Pop $0
	${NSD_CreateLabel} 0 0 100% 22u "ReviveNext is free. If it brings your games back to life, you can support its development:"
	Pop $0
	${NSD_CreateLink} 0 28u 100% 13u "Buy me a coffee"
	Pop $LinkCoffee
	${NSD_OnClick} $LinkCoffee OnCoffeeLink
	${NSD_CreateLink} 0 46u 100% 13u "Sponsor on GitHub"
	Pop $LinkSponsor
	${NSD_OnClick} $LinkSponsor OnSponsorLink
	${NSD_CreateLabel} 0 64u 100% 12u "Both links open in your browser."
	Pop $0
	${NSD_CreateCheckbox} 0 88u 100% 12u "Create Start Menu shortcut"
	Pop $ShortcutStartMenu
	${NSD_Check} $ShortcutStartMenu
	${NSD_CreateCheckbox} 0 104u 100% 12u "Create desktop shortcut"
	Pop $ShortcutDesktop
	${NSD_Check} $ShortcutDesktop
	${NSD_CreateCheckbox} 0 120u 100% 12u "Launch ReviveNext when installation finishes"
	Pop $LaunchCheckbox
	${NSD_Check} $LaunchCheckbox
	nsDialogs::Show
FunctionEnd

Function SupportPageLeave
	; Capture the checkbox states here - the controls are gone once
	; the page closes, and the actions run after the install (see
	; DonePageLeave).
	${NSD_GetState} $ShortcutStartMenu $ShortcutStartMenu
	${NSD_GetState} $ShortcutDesktop $ShortcutDesktop
	${NSD_GetState} $LaunchCheckbox $LaunchCheckbox
FunctionEnd

Section "ReviveNext"
  SetOutPath "$INSTDIR"
  File "${ROOT}\build\Release\ReviveNext.exe"
  SetOutPath "$INSTDIR\Resources"
  File /r "${ROOT}\build\Release\Resources\*.*"

  ; Shortcuts are created from the post-install page checkboxes
  ; (DonePageLeave) rather than unconditionally here.

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\ReviveNext" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "DisplayName" "ReviveNext"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "DisplayIcon" '"$INSTDIR\ReviveNext.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ReviveNext" "Publisher" "Brian Gilbert"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\ReviveNext.lnk"
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
