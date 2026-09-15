; NAMp Windows installer — one installer for everything this project ships.
;
; ONE DOWNLOAD, ONE INSTALLER, THREE COMPONENTS: the plug-in, the five pedals and the standalone.
; They are one amp, released together and versioned together, so asking a user to find and run
; three installers would be asking them to assemble the release themselves — and would let a
; person end up running a 0.3.0 plug-in beside a 0.2.9 standalone. Each component can still be
; deselected on the components page; what is not offered is the chance to get them out of step.
;
; WHY AN INSTALLER AT ALL. A VST3 plug-in is installed by copying a folder, and the ZIP still
; contains those folders for anyone who would rather do it by hand. What the installer buys is the
; three things copying by hand gets wrong: putting bundles somewhere a host actually looks, putting
; the standalone somewhere with a Start Menu entry, and taking all of it away again. Windows has no
; equivalent of ~/.vst3 that everyone knows, the per-user path is buried four levels inside
; %LOCALAPPDATA%, and an old copy left in the other location shows up as a duplicate in the
; plug-in list.
;
; NO WINDOWS MACHINE IS INVOLVED IN BUILDING THIS. makensis is a native Linux binary; it links one
; of NSIS's prebuilt PE stubs and appends the compressed payload, so producing NAMp-install.exe
; needs neither Wine nor a cross compiler. Wine is used afterwards, to run it — a smoke test, not
; the gate.
;
; LICENSING, BECAUSE ONE OF THE THREE COMPONENTS IS NOT MIT. namp-rack.exe is built with the ASIO
; SDK compiled in and is conveyed under GPLv3; the plug-in and the pedals are MIT. Putting them in
; one installer is an aggregate in the sense of GPLv3 section 5 — separate and independent works on
; one distribution medium, not combined into a larger program — so the MIT parts stay MIT and are
; available under those terms whether or not the standalone is installed. The licence page shows
; the MIT text and LICENSE-windows-asio.txt, installed beside the program, explains the split in
; one page. Which document covers what is stated rather than left to be worked out.
;
; INVOKED BY scripts/makedist-windows.sh, which supplies every path:
;
;   makensis -DVERSION=x.y.z -DVERSION4=x.y.z.0 \
;            -DPLUGIN_DIR=<staged>/plugin/NAMp-rations.vst3 \
;            -DPEDALS_DIR=<staged>/pedals -DRACK_EXE=<staged>/rack/namp-rack.exe \
;            -DDOC_DIR=<staged> -DPEDALS_NSH=<generated fragment> \
;            -DOUTFILE=<staged>/NAMp-install.exe installer/namp.nsi
;
; THE PEDAL LIST IS GENERATED, NEVER WRITTEN HERE. PEDALS_NSH is a fragment the packaging script
; writes from what was actually staged, so a sixth pedal upstream ships without anyone remembering
; to edit this file — and a list that quietly stopped matching, which is how a release starts
; installing four of five, cannot happen.
;
; THIS IS A 32-BIT INSTALLER INSTALLING 64-BIT BINARIES, deliberately. NSIS 3.11 does ship
; amd64-unicode stubs, but the 32-bit stub is the path every audio-plug-in installer on Windows has
; taken for twenty years, and the only thing the 64-bit one would save is the two lines below that
; spell out the view and the folder. Both of those have to be explicit anyway, or a 32-bit process
; silently gets WOW6432Node and \Program Files (x86):
;
;   SetRegView 64   for every registry access, in the installer AND the uninstaller
;   $COMMONFILES64  rather than $COMMONFILES, and $PROGRAMFILES64 rather than $PROGRAMFILES
;
; The installer is NOT code-signed, so Windows SmartScreen will warn about it. That is why the
; plain bundles stay in the ZIP beside it.

Unicode true
Target x86-unicode

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "Sections.nsh"

!ifndef VERSION
  !error "VERSION is not defined - pass -DVERSION=x.y.z"
!endif
!ifndef VERSION4
  !error "VERSION4 is not defined - pass -DVERSION4=x.y.z.0 (VIProductVersion needs four parts)"
!endif
!ifndef PLUGIN_DIR
  !error "PLUGIN_DIR is not defined - pass -DPLUGIN_DIR=<path to the staged NAMp-rations.vst3>"
!endif
!ifndef PEDALS_DIR
  !error "PEDALS_DIR is not defined - pass -DPEDALS_DIR=<path to the staged pedals directory>"
!endif
!ifndef RACK_EXE
  !error "RACK_EXE is not defined - pass -DRACK_EXE=<path to the staged namp-rack.exe>"
!endif
!ifndef DOC_DIR
  !error "DOC_DIR is not defined - pass -DDOC_DIR=<path holding LICENSE, NOTICE, INSTALL.txt>"
!endif
!ifndef PEDALS_NSH
  !error "PEDALS_NSH is not defined - pass -DPEDALS_NSH=<generated pedal list fragment>"
!endif
!ifndef OUTFILE
  !define OUTFILE "NAMp-install.exe"
!endif

; Two names, deliberately. APPNAME is DISPLAY text - what the user reads on the pages and in
; "Apps & features". APPID is the FILESYSTEM and REGISTRY identity - the install directory, the
; uninstaller's filename and both registry keys. Anything that becomes a path must use an ID
; rather than a display name: a VST3 bundle folder in particular has to match the DLL inside it
; exactly or no host will load the plug-in.
!define APPNAME "NAMp"
!define APPID "NAMp"
!define PLUGID "NAMp-rations"
!define RACKID "namp-rack"
!define PUBLISHER "rations"
!define ABOUTURL "https://github.com/rations/NAMp-rations"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
BrandingText "${APPNAME} ${VERSION}"
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

; "highest" rather than "admin": an administrator is elevated and gets the machine-wide Common
; Files install, and everybody else still gets a working per-user install instead of a UAC prompt
; they cannot answer. Which of the two happened is decided in .onInit and shown on the directory
; page.
RequestExecutionLevel highest

VIProductVersion "${VERSION4}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} ${VERSION} installer"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "MIT, except namp-rack.exe which is GPLv3. See NOTICE."

Var Vst3Dir  ; the VST3 folder the user picks; the bundles are created inside it
Var AppDir   ; where the standalone, the uninstaller and the licence files live
Var AllUsers ; 1 = machine-wide (elevated), 0 = this user only
Var OtherDir ; the standard VST3 location we are NOT installing into, checked for a stale copy
Var Installed ; how many components actually installed, for the final DetailPrint

;--------------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs NAMp: a four-channel amp head built on Neural Amp Modeler captures, as a plug-in for your DAW and as a standalone program, plus five pedals.$\r$\n$\r$\nEach channel loads its own bank of captures, and its dial sweeps that whole bank continuously - so a channel captured at ascending gain settings gives you that amp's gain control back. Exactly one channel sounds at a time, and the change is instant and silent, by bat switch or MIDI footswitch.$\r$\n$\r$\nAround them: Threshold, Bass, Middle and Treble, and a cabinet page for one or two impulse responses.$\r$\n$\r$\nNo captures are included - it plays yours, four banks of them.$\r$\n$\r$\nThere is nothing else to install: cairo, FreeType, libpng, zlib and the GCC runtime are linked in."
!insertmacro MUI_PAGE_WELCOME

!insertmacro MUI_PAGE_LICENSE "${DOC_DIR}\LICENSE"

!define MUI_COMPONENTSPAGE_TEXT_TOP "The plug-in and the standalone are the same amp. Install both, or only the one you want."
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ComponentsLeave
!insertmacro MUI_PAGE_COMPONENTS

!define MUI_PAGE_HEADER_TEXT "Choose the VST3 folder"
!define MUI_PAGE_HEADER_SUBTEXT "The plug-in and the pedals are installed into it, each as its own folder."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Hosts search these two folders, in this order:$\r$\n$\r$\n    %LOCALAPPDATA%\Programs\Common\VST3      (just you, no administrator rights)$\r$\n    C:\Program Files\Common Files\VST3      (every user, needs administrator rights)$\r$\n$\r$\nThe one below was chosen for you. Change it only if your host is set up to look somewhere else.$\r$\n$\r$\nThe standalone does not go here - it is installed as an ordinary program, with a Start Menu entry."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "VST3 folder"
!define MUI_DIRECTORYPAGE_VARIABLE $Vst3Dir
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "${APPNAME} is installed"
!define MUI_FINISHPAGE_TEXT "Rescan plug-ins in your DAW to pick up the amp and the pedals.$\r$\n$\r$\nClick $\"Captures, MIDI, Settings$\", top right, to load .nam captures. An empty channel is silent.$\r$\n$\r$\nThe standalone is in your Start Menu as ${APPNAME} Rack."
!define MUI_FINISHPAGE_SHOWREADME "$AppDir\INSTALL.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the notes on captures"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

;--------------------------------------------------------------------------
Function .onInit
    ${IfNot} ${RunningX64}
        ; /SD, here and below: without it a message box is shown even under /S, and an unattended
        ; install stops dead on a dialog nobody is watching.
        MessageBox MB_ICONSTOP|MB_OK "${APPNAME} is 64-bit only, and this is a 32-bit Windows.$\r$\n$\r$\nThere is no 32-bit build." /SD IDOK
        Abort
    ${EndIf}

    ; SetRegView governs every registry access in this script, including the uninstall entry.
    ; Without it a 32-bit installer writes into WOW6432Node, where 64-bit "Apps & features" does
    ; not look and the entry never appears.
    SetRegView 64

    StrCpy $Installed 0

    ClearErrors
    UserInfo::GetAccountType
    Pop $0
    ${If} ${Errors}
        ; Win9x-era fallback path; treat as unprivileged rather than guessing.
        StrCpy $0 "User"
    ${EndIf}

    ; $LOCALAPPDATA FOLLOWS SetShellVarContext, and in "all" context it is not a per-user folder at
    ; all: measured under Wine, current -> C:\users\<name>\AppData\Local but all -> C:\ProgramData.
    ; So the per-user paths are read here, in "current" context, BEFORE the elevated branch
    ; switches to "all" — otherwise the elevated install looks for the other copy under
    ; ProgramData, never finds it, and silently leaves the duplicate the check exists to catch.
    ; $COMMONFILES64 and $PROGRAMFILES64 do not depend on the context.
    SetShellVarContext current
    StrCpy $1 "$LOCALAPPDATA\Programs\Common\VST3"
    StrCpy $2 "$LOCALAPPDATA\Programs\${APPID}"

    ${If} $0 == "Admin"
        StrCpy $AllUsers 1
        SetShellVarContext all
        StrCpy $Vst3Dir "$COMMONFILES64\VST3"
        StrCpy $AppDir "$PROGRAMFILES64\${APPID}"
        StrCpy $OtherDir "$1"
    ${Else}
        StrCpy $AllUsers 0
        StrCpy $Vst3Dir "$1"
        StrCpy $AppDir "$2"
        StrCpy $OtherDir "$COMMONFILES64\VST3"
    ${EndIf}

    ; $INSTDIR is the uninstaller's home and the program's, not any bundle's. The bundles are
    ; created under $Vst3Dir, whose name the directory page edits directly.
    StrCpy $INSTDIR "$AppDir"
FunctionEnd

;--------------------------------------------------------------------------
Section "NAMp Rations - the amp as a plug-in (VST3)" SecPlugin
    ; A stale copy in the OTHER standard location is not harmless: hosts scan both, so it comes
    ; back as a second "NAMp Rations" in the plug-in list, and which of the two a project loads is
    ; not something the user controls. Offer to remove it. Both paths are fixed and known, which
    ; is the only reason a recursive delete is acceptable here at all.
    ${If} ${FileExists} "$OtherDir\${PLUGID}.vst3\Contents\*.*"
        ; /SD IDNO: an unattended install must never delete a folder on its own.
        MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON1 \
            "Another copy of NAMp Rations is already installed at:$\r$\n$\r$\n$OtherDir\${PLUGID}.vst3$\r$\n$\r$\nYour host would list it twice. Remove that copy?" \
            /SD IDNO IDNO +2
        RMDir /r "$OtherDir\${PLUGID}.vst3"
    ${EndIf}

    ; Replace rather than merge. An upgrade that only overwrites would leave behind art and fonts
    ; a later version had dropped, and the editor picks up whatever is on disk.
    ${If} ${FileExists} "$Vst3Dir\${PLUGID}.vst3\Contents\*.*"
        DetailPrint "Removing the previous $Vst3Dir\${PLUGID}.vst3"
        RMDir /r "$Vst3Dir\${PLUGID}.vst3"
    ${EndIf}

    SetOutPath "$Vst3Dir\${PLUGID}.vst3"
    File /r "${PLUGIN_DIR}\*"

    WriteRegStr SHCTX "Software\${APPID}" "BundlePath" "$Vst3Dir\${PLUGID}.vst3"
    IntOp $Installed $Installed + 1
    DetailPrint "Installed $Vst3Dir\${PLUGID}.vst3"
SectionEnd

Section "Rations Pedals - five pedals (VST3)" SecPedals
    ; Ordinary plug-ins, installed the ordinary way. They are not the standalone's: they go where
    ; every host looks, so the DAW running NAMp Rations finds them too. The list below is
    ; GENERATED from what was staged — see the header — and each bundle is replaced rather than
    ; merged, for the same reason the plug-in is.
    !include "${PEDALS_NSH}"

    WriteRegStr SHCTX "Software\${APPID}" "PedalsPath" "$Vst3Dir"
    IntOp $Installed $Installed + 1
SectionEnd

Section "NAMp Rack - the amp standalone, with a plug-in rack" SecRack
    SetOutPath "$AppDir"
    File "${RACK_EXE}"

    CreateDirectory "$SMPROGRAMS\${APPNAME}"
    CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME} Rack.lnk" "$AppDir\${RACKID}.exe" "" "$AppDir\${RACKID}.exe" 0

    WriteRegStr SHCTX "Software\${APPID}" "RackPath" "$AppDir\${RACKID}.exe"
    IntOp $Installed $Installed + 1
    DetailPrint "Installed $AppDir\${RACKID}.exe"
SectionEnd

; NOT a component: the licence files and the uninstaller are not optional, and neither is the
; uninstall entry. -SecCommon rather than SecCommon — a section whose name begins with a hyphen is
; hidden from the components page — and it is LAST so it runs after the three above have counted
; themselves.
Section "-SecCommon"
    ; Nothing goes inside a bundle that a hand-copied one does not also have, so an installed
    ; NAMp-rations.vst3 and one dragged out of the ZIP are byte for byte the same folder. The
    ; uninstaller and the licence files live beside the program instead.
    SetOutPath "$AppDir"
    File "${DOC_DIR}\LICENSE"
    File "${DOC_DIR}\NOTICE"
    File "${DOC_DIR}\INSTALL.txt"
    File "${DOC_DIR}\README.md"

    ; THE GPLv3 PAPERWORK TRAVELS WITH THE PROGRAM IT COVERS, and the two halves of it are
    ; conditional on DIFFERENT things — which is the whole reason they are two defines rather than
    ; one.
    ;
    ; GPLV3_DOCS is set whenever the standalone was built with ASIO in it, so section 4's "give all
    ; recipients a copy of this License along with the Program" is satisfied by the fact that the
    ; program is GPLv3, full stop. It does not depend on whether the Corresponding Source happened
    ; to be assembled: an unpublishable build still puts the licence on disk, because the condition
    ; the licence attaches to is the presence of GPLv3 code and nothing else.
    ;
    ; CS_DOC is set only when scripts/corresponding-source.sh actually ran, because 6(d)'s "clear
    ; directions" have to point at something. An archive without it is marked -NOSOURCE and must
    ; not be published; installing a directions file that pointed nowhere would be worse than
    ; installing none.
    ;
    ; A WASAPI-only build defines neither: no ASIO, no GPLv3 code, nothing to accompany.
    ${If} ${SectionIsSelected} ${SecRack}
        !ifdef GPLV3_DOCS
            File "${DOC_DIR}\COPYING.GPL-3"
            File "${DOC_DIR}\LICENSE-windows-asio.txt"
        !endif
        !ifdef CS_DOC
            File "${DOC_DIR}\CORRESPONDING-SOURCE.txt"
        !endif
    ${EndIf}

    WriteUninstaller "$AppDir\Uninstall ${APPID}.exe"

    WriteRegStr SHCTX "Software\${APPID}" "AppPath" "$AppDir"
    WriteRegStr SHCTX "Software\${APPID}" "Version" "${VERSION}"

    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayName" "${APPNAME} ${VERSION}"
    WriteRegStr SHCTX "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr SHCTX "${UNINSTKEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr SHCTX "${UNINSTKEY}" "URLInfoAbout" "${ABOUTURL}"
    WriteRegStr SHCTX "${UNINSTKEY}" "InstallLocation" "$AppDir"
    WriteRegStr SHCTX "${UNINSTKEY}" "UninstallString" "$\"$AppDir\Uninstall ${APPID}.exe$\""
    WriteRegStr SHCTX "${UNINSTKEY}" "QuietUninstallString" "$\"$AppDir\Uninstall ${APPID}.exe$\" /S"
    WriteRegDWORD SHCTX "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD SHCTX "${UNINSTKEY}" "NoRepair" 1

    ${GetSize} "$AppDir" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD SHCTX "${UNINSTKEY}" "EstimatedSize" "$0"

    DetailPrint "$Installed of 3 components installed"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecPlugin} "The amp as a VST3 plug-in for your DAW, with a five-pedal pedalboard built into its panel. MIT."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecPedals} "Boost, Chorus, Flanger, Delay and Reverb as five separate VST3 plug-ins, for the standalone and for any other host. MIT."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecRack} "The same amp in its own window, with a rack that hosts your other VST3 plug-ins before and after it. Uses ASIO, and is therefore under the GPLv3 rather than MIT - see LICENSE-windows-asio.txt."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; AN INSTALLER THAT INSTALLS NOTHING still writes an uninstall entry and still tells the user it
; succeeded, which is a worse outcome than refusing to continue. Checked on LEAVING the components
; page rather than at the start of the first section, so the user is told while the checkboxes are
; still in front of them. It sits here, below the sections, because ${SecPlugin} and the other two
; are compile-time defines created BY their Section lines - a function above them cannot name one.
Function ComponentsLeave
    SectionGetFlags ${SecPlugin} $0
    IntOp $0 $0 & ${SF_SELECTED}
    SectionGetFlags ${SecPedals} $1
    IntOp $1 $1 & ${SF_SELECTED}
    SectionGetFlags ${SecRack} $2
    IntOp $2 $2 & ${SF_SELECTED}
    IntOp $0 $0 + $1
    IntOp $0 $0 + $2
    ${If} $0 == 0
        MessageBox MB_ICONEXCLAMATION|MB_OK "Nothing is selected, so there is nothing to install.$\r$\n$\r$\nTick at least one component, or cancel." /SD IDOK
        Abort
    ${EndIf}
FunctionEnd

;--------------------------------------------------------------------------
; The uninstaller has to work out for itself which of the two installs it is, because it is one
; binary written by both paths. It reads HKLM first: a machine-wide install is the one that needs
; elevation, and getting it wrong that way fails loudly rather than silently leaving files behind.
Function un.onInit
    SetRegView 64

    SetShellVarContext all
    ReadRegStr $0 HKLM "Software\${APPID}" "AppPath"
    ${If} $0 == ""
        SetShellVarContext current
        ReadRegStr $0 HKCU "Software\${APPID}" "AppPath"
    ${EndIf}
    StrCpy $AppDir $0
    StrCpy $INSTDIR $0

    ReadRegStr $Vst3Dir SHCTX "Software\${APPID}" "PedalsPath"
FunctionEnd

Section "Uninstall"
    ; EVERY PATH HERE CAME OUT OF THE REGISTRY, so none of them is trusted the way a path this
    ; script built would be: each is removed only if it still looks like the thing it claims to be.
    ; A corrupted or hand-edited value must not turn this into rm -rf.
    ReadRegStr $0 SHCTX "Software\${APPID}" "BundlePath"
    ${If} $0 != ""
    ${AndIf} ${FileExists} "$0\Contents\x86_64-win\${PLUGID}.vst3"
        RMDir /r "$0"
        DetailPrint "Removed $0"
    ${EndIf}

    ; The pedals, by the same test: a folder is removed only when it holds the DLL that gives it
    ; its name. The list is the generated one, so it matches what was installed.
    ${If} $Vst3Dir != ""
        !define UNINSTALLING
        !include "${PEDALS_NSH}"
        !undef UNINSTALLING
    ${EndIf}

    ReadRegStr $0 SHCTX "Software\${APPID}" "RackPath"
    ${If} $0 != ""
    ${AndIf} ${FileExists} "$0"
        Delete "$0"
        DetailPrint "Removed $0"
    ${EndIf}
    Delete "$SMPROGRAMS\${APPNAME}\${APPNAME} Rack.lnk"
    RMDir "$SMPROGRAMS\${APPNAME}"

    ${If} $AppDir != ""
        Delete "$AppDir\LICENSE"
        Delete "$AppDir\NOTICE"
        Delete "$AppDir\INSTALL.txt"
        Delete "$AppDir\README.md"
        Delete "$AppDir\COPYING.GPL-3"
        Delete "$AppDir\LICENSE-windows-asio.txt"
        Delete "$AppDir\CORRESPONDING-SOURCE.txt"
        Delete "$AppDir\Uninstall ${APPID}.exe"
        RMDir "$AppDir"
    ${EndIf}

    DeleteRegKey SHCTX "${UNINSTKEY}"
    DeleteRegKey SHCTX "Software\${APPID}"
SectionEnd
