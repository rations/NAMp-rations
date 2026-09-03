; Rations Windows installer — built by makensis running natively on Linux.
;
; WHY AN INSTALLER AT ALL. A VST3 plug-in is installed by copying a folder, and
; the ZIP still contains that folder for anyone who would rather do it by hand.
; What the installer buys is the two things copying by hand gets wrong: putting
; the bundle somewhere a host actually looks, and taking it away again. Windows
; has no equivalent of ~/.vst3 that everyone knows, the per-user path is buried
; four levels inside %LOCALAPPDATA%, and an old copy left in the other location
; shows up as a second Rations in the plug-in list.
;
; NO WINDOWS MACHINE IS INVOLVED IN BUILDING THIS. makensis is a native Linux
; binary; it links one of NSIS's prebuilt PE stubs and appends the compressed
; payload, so producing Rations-install.exe needs neither Wine nor a cross
; compiler. Wine is used afterwards, to run it — a smoke test, not the gate.
;
; INVOKED BY scripts/makedist-windows.sh, which supplies every path:
;
;   makensis -DVERSION=0.1.0 -DVERSION4=0.1.0.0 \
;            -DBUNDLE_DIR=<staged>/Rations.vst3 -DDOC_DIR=<staged> \
;            -DOUTFILE=<staged>/Rations-install.exe installer/rations.nsi
;
; THIS IS A 32-BIT INSTALLER INSTALLING A 64-BIT PLUG-IN, deliberately. NSIS
; 3.11 does ship amd64-unicode stubs, but the 32-bit stub is the path every
; audio-plug-in installer on Windows has taken for twenty years, and the only
; thing the 64-bit one would save is the two lines below that spell out the
; view and the folder. Both of those have to be explicit anyway, or a 32-bit
; process silently gets WOW6432Node and \Program Files (x86):
;
;   SetRegView 64   for every registry write, and again in the uninstaller
;   $COMMONFILES64  rather than $COMMONFILES
;
; The installer is NOT code-signed, so Windows SmartScreen will warn about it.
; That is why the plain bundle stays in the ZIP beside it.

Unicode true
Target x86-unicode

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !error "VERSION is not defined - pass -DVERSION=x.y.z"
!endif
!ifndef VERSION4
  !error "VERSION4 is not defined - pass -DVERSION4=x.y.z.0 (VIProductVersion needs four parts)"
!endif
!ifndef BUNDLE_DIR
  !error "BUNDLE_DIR is not defined - pass -DBUNDLE_DIR=<path to the staged Rations.vst3>"
!endif
!ifndef DOC_DIR
  !error "DOC_DIR is not defined - pass -DDOC_DIR=<path holding LICENSE, NOTICE, INSTALL.txt>"
!endif
!ifndef OUTFILE
  !define OUTFILE "Rations-install.exe"
!endif

!define APPNAME "Rations"
!define PUBLISHER "rations"
!define ABOUTURL "https://github.com/rations/Rations"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
BrandingText "${APPNAME} ${VERSION}"
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

; "highest" rather than "admin": an administrator is elevated and gets the
; machine-wide Common Files install, and everybody else still gets a working
; per-user install instead of a UAC prompt they cannot answer. Which of the two
; happened is decided in .onInit and shown on the directory page.
RequestExecutionLevel highest

VIProductVersion "${VERSION4}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} ${VERSION} VST3 installer"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "MIT. See NOTICE for third-party attribution."

Var Vst3Dir  ; the VST3 folder the user picks; $INSTDIR is always $Vst3Dir\Rations.vst3
Var AppDir   ; where the uninstaller and the licence files live, outside the bundle
Var AllUsers ; 1 = machine-wide (elevated), 0 = this user only
Var OtherDir ; the standard location we are NOT installing into, checked for a stale copy

;--------------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs the ${APPNAME} VST3 plug-in.$\r$\n$\r$\n${APPNAME} is a four-channel amp head built on Neural Amp Modeler captures. Each channel loads its own bank of captures, and its dial sweeps that whole bank continuously - so a channel captured at ascending gain settings gives you that amp's gain control back. Exactly one channel sounds at a time, and the change is instant and silent, by bat switch or MIDI footswitch.$\r$\n$\r$\nAround them: Threshold, Bass, Middle and Treble, a cabinet page for one or two impulse responses, and a board of five pedals.$\r$\n$\r$\nNo captures are included - it plays yours, four banks of them.$\r$\n$\r$\nThere is nothing else to install: cairo, FreeType, libpng, zlib and the GCC runtime are all linked into the plug-in."
!insertmacro MUI_PAGE_WELCOME

!insertmacro MUI_PAGE_LICENSE "${DOC_DIR}\LICENSE"

!define MUI_PAGE_HEADER_TEXT "Choose the VST3 folder"
!define MUI_PAGE_HEADER_SUBTEXT "A folder called Rations.vst3 is created inside it."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Hosts search these two folders, in this order:$\r$\n$\r$\n    %LOCALAPPDATA%\Programs\Common\VST3      (just you, no administrator rights)$\r$\n    C:\Program Files\Common Files\VST3      (every user, needs administrator rights)$\r$\n$\r$\nThe one below was chosen for you. Change it only if your host is set up to look somewhere else."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "VST3 folder"
!define MUI_DIRECTORYPAGE_VARIABLE $Vst3Dir
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE OnDirectoryLeave
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "${APPNAME} is installed"
; Kept short deliberately: the finish page's text area is fixed, and MUI clips
; rather than scrolls — a longer version ran under the "show readme" checkbox.
; MEASURED, twice: the area is two lines of about 53 characters after the first
; paragraph, and naming the settings button here overran it at 149 characters
; and had to come back to 98. Anything longer belongs in INSTALL.txt, which the
; checkbox under this text opens.
!define MUI_FINISHPAGE_TEXT "Rescan plug-ins in your DAW to pick it up.$\r$\n$\r$\nClick $\"Captures, MIDI, Settings$\", top right, to load .nam captures. An empty channel is silent."
!define MUI_FINISHPAGE_SHOWREADME "$AppDir\INSTALL.txt"
; The checkbox's own label is ONE line of a control MUI sizes for one line, and
; it is narrower than the text above it because the box and its margin come out
; of the same width: 51 characters wrapped and the second line was cut in half,
; which is how this shipped until it was looked at under Wine. Measured, the
; wrap fell after 44, so this is kept well under it.
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the notes on captures"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

;--------------------------------------------------------------------------
Function .onInit
    ${IfNot} ${RunningX64}
        ; /SD, here and below: without it a message box is shown even under /S,
        ; and an unattended install stops dead on a dialog nobody is watching.
        MessageBox MB_ICONSTOP|MB_OK "${APPNAME} is 64-bit only, and this is a 32-bit Windows.$\r$\n$\r$\nThere is no 32-bit build." /SD IDOK
        Abort
    ${EndIf}

    ; SetRegView governs every registry access in this script, including the
    ; uninstall entry. Without it a 32-bit installer writes into WOW6432Node,
    ; where 64-bit "Apps & features" does not look and the entry never appears.
    SetRegView 64

    ClearErrors
    UserInfo::GetAccountType
    Pop $0
    ${If} ${Errors}
        ; Win9x-era fallback path; treat as unprivileged rather than guessing.
        StrCpy $0 "User"
    ${EndIf}

    ; $LOCALAPPDATA FOLLOWS SetShellVarContext, and in "all" context it is not a
    ; per-user folder at all: measured under Wine, current -> C:\users\<name>\
    ; AppData\Local but all -> C:\ProgramData. So the per-user paths are read
    ; here, in "current" context, BEFORE the elevated branch switches to "all" —
    ; otherwise the elevated install looks for the other copy under ProgramData,
    ; never finds it, and silently leaves the duplicate the check exists to
    ; catch. $COMMONFILES64 and $PROGRAMFILES64 do not depend on the context.
    SetShellVarContext current
    StrCpy $1 "$LOCALAPPDATA\Programs\Common\VST3"
    StrCpy $2 "$LOCALAPPDATA\Programs\${APPNAME}"

    ${If} $0 == "Admin"
        StrCpy $AllUsers 1
        SetShellVarContext all
        StrCpy $Vst3Dir "$COMMONFILES64\VST3"
        StrCpy $AppDir "$PROGRAMFILES64\${APPNAME}"
        StrCpy $OtherDir "$1\${APPNAME}.vst3"
    ${Else}
        StrCpy $AllUsers 0
        StrCpy $Vst3Dir "$1"
        StrCpy $AppDir "$2"
        StrCpy $OtherDir "$COMMONFILES64\VST3\${APPNAME}.vst3"
    ${EndIf}

    StrCpy $INSTDIR "$Vst3Dir\${APPNAME}.vst3"
FunctionEnd

; The directory page edits $Vst3Dir, not $INSTDIR. Deriving $INSTDIR here is
; what guarantees the bundle is always created as <chosen folder>\Rations.vst3 —
; if the page wrote $INSTDIR directly, a user who browsed to D:\Plugins would
; get Contents\ spilled loose into D:\Plugins and no host would load it.
Function OnDirectoryLeave
    StrCpy $INSTDIR "$Vst3Dir\${APPNAME}.vst3"
FunctionEnd

;--------------------------------------------------------------------------
Section "${APPNAME} VST3 plug-in" SecPlugin
    SectionIn RO

    ; A stale copy in the OTHER standard location is not harmless: hosts scan
    ; both, so it comes back as a second "Rations" in the plug-in list, and
    ; which of the two a project loads is not something the user controls.
    ; Offer to remove it. Both paths are fixed and known, which is the only
    ; reason a recursive delete is acceptable here at all.
    ${If} ${FileExists} "$OtherDir\Contents\*.*"
        ; /SD IDNO: an unattended install must never delete a folder on its own.
        MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON1 \
            "Another copy of ${APPNAME} is already installed at:$\r$\n$\r$\n$OtherDir$\r$\n$\r$\nYour host would list it twice. Remove that copy?" \
            /SD IDNO IDNO +2
        RMDir /r "$OtherDir"
    ${EndIf}

    ; Replace rather than merge. An upgrade that only overwrites would leave
    ; behind art and fonts a later version had dropped, and the editor picks up
    ; whatever is on disk.
    ${If} ${FileExists} "$INSTDIR\Contents\*.*"
        DetailPrint "Removing the previous $INSTDIR"
        RMDir /r "$INSTDIR"
    ${EndIf}

    SetOutPath "$INSTDIR"
    File /r "${BUNDLE_DIR}\*"

    ; Nothing goes inside the bundle that a hand-copied one does not also have,
    ; so an installed Rations.vst3 and one dragged out of the ZIP are byte for
    ; byte the same folder. The uninstaller and the licence files live beside it.
    SetOutPath "$AppDir"
    File "${DOC_DIR}\LICENSE"
    File "${DOC_DIR}\NOTICE"
    File "${DOC_DIR}\INSTALL.txt"
    WriteUninstaller "$AppDir\Uninstall ${APPNAME}.exe"

    WriteRegStr SHCTX "Software\${APPNAME}" "BundlePath" "$INSTDIR"
    WriteRegStr SHCTX "Software\${APPNAME}" "AppPath" "$AppDir"
    WriteRegStr SHCTX "Software\${APPNAME}" "Version" "${VERSION}"

    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayName" "${APPNAME} ${VERSION}"
    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr SHCTX "${UNINSTKEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr SHCTX "${UNINSTKEY}" "URLInfoAbout" "${ABOUTURL}"
    WriteRegStr SHCTX "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr SHCTX "${UNINSTKEY}" "UninstallString" "$\"$AppDir\Uninstall ${APPNAME}.exe$\""
    WriteRegStr SHCTX "${UNINSTKEY}" "QuietUninstallString" "$\"$AppDir\Uninstall ${APPNAME}.exe$\" /S"
    WriteRegDWORD SHCTX "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD SHCTX "${UNINSTKEY}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD SHCTX "${UNINSTKEY}" "EstimatedSize" "$0"

    DetailPrint "Installed $INSTDIR"
SectionEnd

;--------------------------------------------------------------------------
; The uninstaller has to work out for itself which of the two installs it is,
; because it is one binary written by both paths. It reads HKLM first: a
; machine-wide install is the one that needs elevation, and getting it wrong
; that way fails loudly rather than silently leaving the bundle behind.
Function un.onInit
    SetRegView 64

    SetShellVarContext all
    ReadRegStr $0 HKLM "Software\${APPNAME}" "BundlePath"
    ${If} $0 == ""
        SetShellVarContext current
        ReadRegStr $0 HKCU "Software\${APPNAME}" "BundlePath"
    ${EndIf}
    StrCpy $INSTDIR $0

    ReadRegStr $0 SHCTX "Software\${APPNAME}" "AppPath"
    StrCpy $AppDir $0
FunctionEnd

Section "Uninstall"
    ; $INSTDIR came out of the registry, so it is not trusted the way a path
    ; this script built would be: only remove it if it still looks like our
    ; bundle. A corrupted or hand-edited value must not turn this into rm -rf.
    ${If} $INSTDIR != ""
    ${AndIf} ${FileExists} "$INSTDIR\Contents\x86_64-win\${APPNAME}.vst3"
        RMDir /r "$INSTDIR"
        DetailPrint "Removed $INSTDIR"
    ${Else}
        DetailPrint "No ${APPNAME}.vst3 bundle found at $INSTDIR - nothing to remove there"
    ${EndIf}

    ${If} $AppDir != ""
        Delete "$AppDir\LICENSE"
        Delete "$AppDir\NOTICE"
        Delete "$AppDir\INSTALL.txt"
        Delete "$AppDir\Uninstall ${APPNAME}.exe"
        RMDir "$AppDir"
    ${EndIf}

    DeleteRegKey SHCTX "${UNINSTKEY}"
    DeleteRegKey SHCTX "Software\${APPNAME}"
SectionEnd
