#!/usr/bin/env bash
# Build the NAMp release for 64-bit Windows: ONE package, with ONE installer that installs
# everything this project ships.
#
# ONE DOWNLOAD, THREE THINGS IN IT, AND ONE NAMp-install.exe THAT INSTALLS THEM ALL.
#
#   plugin/NAMp-rations.vst3   the amp as a plug-in for a DAW
#   rack/namp-rack.exe         the same amp standalone, with a rack for other people's plug-ins
#   pedals/Rations*.vst3       the five pedals, as ordinary plug-ins any host will find
#
# The installer offers the three as components, so a user who wants only the plug-in gets only the
# plug-in. What they cannot do is end up with halves from different releases: one download, one
# version, and the version is cross-checked below before anything is built.
#
# THE BUNDLES ARE ALSO LOOSE IN THE ZIP, and that is not a stylistic preference -- it is the
# fallback for a machine whose SmartScreen or antivirus refuses an unsigned installer. Copying a
# .vst3 folder into the VST3 directory is the whole of a manual install.
#
# HOW IT IS ASSEMBLED. Each product builds and gates itself in its own scripts/stage-windows.sh,
# into its own subdirectory of one staging tree. Those assertions are artefact-specific -- imports,
# exports, bundle shape, the ASIO markers, the panel diff -- and stay beside the product that owns
# them. What lives HERE is what is genuinely shared: the version, the GPLv3 obligations, the
# licence files, the installer and the ZIP.
#
# THIS ARCHIVE CONTAINS ONE GPLv3 BINARY AND EVERYTHING ELSE IS MIT.
#
# namp-rack.exe is built with the ASIO SDK compiled in. That SDK is dual-licensed -- proprietary
# Steinberg agreement, or GPLv3 -- and this project takes the GPLv3 arm, so that one binary is
# conveyed under GPLv3. The plug-in, the LV2 bundle, the pedals and everything on Linux contain no
# ASIO code and stay MIT. Every SOURCE file stays MIT and nothing was relicensed: MIT combines into
# a GPLv3 work, so no contributor had to be asked.
#
# PUTTING THEM IN ONE PACKAGE DOES NOT CHANGE THAT. GPLv3 section 5's last paragraph is explicit:
# separate and independent works, which are not by their nature extensions of the covered work and
# are not combined with it into a larger program, are an AGGREGATE when they share a distribution
# medium, and "inclusion of a covered work in an aggregate does not cause this License to apply to
# the other parts". The plug-in and the pedals are separate binaries that a DAW loads without the
# standalone existing; they are not linked into it and do not extend it. So they stay MIT here
# exactly as they are on Linux, and the documents in the archive say which is which rather than
# leaving a user to work it out.
#
# WHAT GPLv3 ASKS FOR IS GATED, NOT PROMISED. Section 4 wants the licence text to travel with the
# binary and section 6 wants the Corresponding Source available to whoever received it. Both are
# assembled and asserted below. An archive that ships the binary without them is a breach on the
# release itself, which is why it is a gate and why -NOSOURCE is in the FILENAME -- where it
# cannot be lost by someone uploading the wrong file a month later.
#
# NO WINDOWS MACHINE IS INVOLVED. The compiler is MinGW-w64 running here and the verification runs
# the cross-built binaries under Wine, which proves they load, pass the SDK validator and draw
# their editor pages the way the Linux build does. It is NOT a substitute for a test on real
# Windows, which is the actual gate before a release goes out -- and the installer in particular is
# covered by no validator and no test in this tree: install it, run what it installed, and
# uninstall it.
#
# Environment:
#   NAMP_SKIP_ASIO=1      configure the standalone WASAPI-only. The whole archive is then MIT and
#                         the GPLv3 paperwork is dropped to match.
#   NAMP_SKIP_CORRESPONDING_SOURCE=1
#                         do not assemble the Corresponding Source tarball. For iterating locally.
#                         The archive is then named -NOSOURCE and MUST NOT be published. It changes
#                         the NAME and one text file; every check runs and the binaries are
#                         identical.
#   NAMP_SKIP_WINE=1      package without the Wine verification. Says so loudly.
#   NAMP_SKIP_INSTALLER=1 package the bundles without NAMp-install.exe.
#   NAMP_NSIS_DIR         the NSIS prefix, defaulting to ~/third_party/nsis (bin/ and share/nsis/).
#                         A makensis on PATH is used if absent.
#   NAMP_BUILD_DIR        the NATIVE Linux build directory, whose panelrender produces the
#                         reference render the Windows one is compared against.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# shellcheck source=dist-common.sh
. "$REPO/scripts/dist-common.sh"

VERSION="$(namp_dist_release_version "$REPO")"

# makensis is a NATIVE Linux binary: it links one of NSIS's prebuilt PE stubs and appends the
# compressed payload, so NAMp-install.exe is produced without Wine and without the cross compiler.
# Prefer an unpacked prefix (bin/ + share/nsis/, which is what `apt-get download nsis nsis-common`
# + `dpkg-deb -x` gives without root); fall back to a system install.
#
# FOUND BEFORE ANYTHING IS BUILT. It used to be looked for at the end, after two cross builds and
# two Wine verifications, which is twenty minutes of work to discover a missing package.
MAKENSIS=""
NSIS_PREFIX="${NAMP_NSIS_DIR:-${RATIONS_NSIS_DIR:-$HOME/third_party/nsis}}"
if [ -x "$NSIS_PREFIX/bin/makensis" ] && [ -d "$NSIS_PREFIX/share/nsis" ]; then
  MAKENSIS="$NSIS_PREFIX/bin/makensis"
  export NSISDIR="$NSIS_PREFIX/share/nsis"
elif command -v makensis >/dev/null; then
  MAKENSIS="makensis"
fi
if [ "${NAMP_SKIP_INSTALLER:-0}" != "1" ] && [ -z "$MAKENSIS" ]; then
  echo "error: makensis not found, so NAMp-install.exe cannot be built." >&2
  echo "  sudo apt install nsis" >&2
  echo "or unpack it without root into \$NAMP_NSIS_DIR (default" >&2
  echo "$NSIS_PREFIX) as bin/makensis and share/nsis/:" >&2
  echo "  apt-get download nsis nsis-common && dpkg-deb -x <each>.deb root/" >&2
  echo "Set NAMP_SKIP_INSTALLER=1 to package the bundles without an installer." >&2
  exit 1
fi

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/pkg"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

# WHAT THE STAGE SCRIPTS REPORT BACK. They are separate processes, so a shell variable set in one
# cannot be read here; these files are how facts cross that boundary. Outside PKGDIR, so neither
# is packaged.
export NAMP_DIST_FACTS_FILE="$STAGEDIR/facts"
export NAMP_DIST_ABI_OVER_FILE="$STAGEDIR/abi-overshoot"
: > "$NAMP_DIST_FACTS_FILE"
: > "$NAMP_DIST_ABI_OVER_FILE"

echo "== NAMp $VERSION, Windows x86_64 =="
echo
echo "-- NAMp Rations (the plug-in) --"
"$REPO/products/rations/scripts/stage-windows.sh" "$PKGDIR"
echo
echo "-- NAMp Rack (the standalone) and the pedals --"
"$REPO/products/rack/scripts/stage-windows.sh" "$PKGDIR"
echo

# EVERY PART ARRIVED. The two stage scripts each assert their own artefacts in detail; what
# neither can assert is that the OTHER one ran. An archive missing a whole product is the one
# failure mode created by splitting the work in two, so it is checked where the two meet.
namp_dist_require_dirs "$PKGDIR" "the release package" \
  plugin/NAMp-rations.vst3 \
  pedals
namp_dist_require_files "$PKGDIR" "the release package" \
  plugin/NAMp-rations.vst3/Contents/x86_64-win/NAMp-rations.vst3 \
  rack/namp-rack.exe \
  pedals/NOTICE

# WHETHER THIS ARCHIVE CONTAINS GPLv3 CODE is read from what the rack's build actually configured,
# reported through the facts file, rather than recomputed from NAMP_SKIP_ASIO here. The two would
# agree today; they would stop agreeing the moment a configure demoted itself, and the question
# "is this binary GPLv3?" is not one to answer from an intention.
WANT_ASIO="$(sed -n 's/^asio=//p' "$NAMP_DIST_FACTS_FILE" | head -1)"
[ -n "$WANT_ASIO" ] ||
  namp_dist_die "the rack's stage script reported no ASIO state, so this script cannot tell
whether the archive it is about to build contains GPLv3 code."

# --- the GPLv3 obligations ---------------------------------------------------
# WHAT THIS SECTION IS FOR. With ASIO compiled in, namp-rack.exe is a combined work containing
# GPLv3 code and is conveyed under GPLv3. Two of that licence's requirements are mechanical, so
# they are machine-checked here rather than remembered:
#
#   section 4  "give all recipients a copy of this License along with the Program"
#              -> COPYING.GPL-3 goes in the archive.
#   section 6  the Complete Corresponding Source must be available to whoever got the binary
#              -> scripts/corresponding-source.sh assembles it, and CORRESPONDING-SOURCE.txt --
#                 the "clear directions" 6(d) asks for -- goes in the archive beside the .exe.
#
# THE MARK IS MEASURED NOW, NOT DECLARED. An earlier version of this named the archive -TESTBUILD
# unless it was TOLD the Steinberg agreement had been signed, because nothing here could check
# that. Under GPLv3 there is nothing to declare and the one thing that matters is something this
# script can actually verify: whether the Corresponding Source for this exact binary exists. So the
# mark tracks a fact. An archive named -NOSOURCE is one whose source was not assembled, and
# publishing it would be the breach.
CS_DIRECTIONS="$PKGDIR/CORRESPONDING-SOURCE.txt"
CS_TARBALL="$REPO/dist/namp-rack-${VERSION}-corresponding-source.tar.gz"
NOSOURCE=""
if [ "$WANT_ASIO" != "ON" ]; then
  # No ASIO, no GPLv3 code, no obligation: a WASAPI-only build is MIT like everything else here.
  echo "WASAPI-only build: the whole archive is MIT, no Corresponding Source obligation."
elif [ "${NAMP_SKIP_CORRESPONDING_SOURCE:-0}" = "1" ]; then
  NOSOURCE="asked for with NAMP_SKIP_CORRESPONDING_SOURCE=1"
elif ! git -C "$REPO" diff --quiet HEAD -- 2>/dev/null; then
  # Corresponding Source is identified by a commit. A dirty tree cannot produce it honestly, so the
  # archive is marked rather than the build refused -- this is the ordinary state while working,
  # and the whole point is that building is never gated.
  NOSOURCE="the working tree has uncommitted changes"
else
  echo
  echo "== Corresponding Source (GPLv3 section 6) =="
  "$REPO/scripts/corresponding-source.sh" "$REPO/dist"
  "$REPO/scripts/corresponding-source.sh" --directions > "$CS_DIRECTIONS"
fi

if [ -n "$NOSOURCE" ]; then
  cat > "$PKGDIR/NOT-A-RELEASE.txt" <<NOTEOF
This archive is NOT a release and MUST NOT be published.

It contains a GPLv3 binary -- namp-rack.exe with the ASIO backend compiled in --
whose Corresponding Source was not assembled, because ${NOSOURCE}.
GPLv3 section 6 requires that source to be available to anyone who receives the
binary, so distributing this archive as it stands would be a licence breach.

The binaries themselves are fine and every check passed. To make a publishable
archive, commit the tree and run scripts/makedist-windows.sh again without
NAMP_SKIP_CORRESPONDING_SOURCE.

The plug-in and the pedals in this archive are MIT and are unaffected -- it is
only the standalone that carries the obligation.
NOTEOF
fi

# --- licence, attribution ----------------------------------------------------
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"
if [ "$WANT_ASIO" = "ON" ]; then
  cp "$REPO/products/rack/COPYING.GPL-3" \
     "$REPO/products/rack/LICENSE-windows-asio.txt" "$PKGDIR/"
fi

# --- the installer's pedal list ----------------------------------------------
# GENERATED FROM WHAT WAS STAGED, never written into the .nsi by hand. The names come from the
# build (the rack's CMake cache declares them and its stage script copies exactly those), so a
# sixth pedal upstream ships without anyone remembering to edit an installer script -- and a list
# that quietly stopped matching, which is how a release starts installing four of five, cannot
# happen. The same fragment is included twice, once by each side, so the install and uninstall
# lists are the same list by construction rather than by two edits staying in step.
PEDALS_NSH="$STAGEDIR/pedals.nsh"
{
  echo "; GENERATED by scripts/makedist-windows.sh from the staged pedals. Do not edit."
  echo "!ifdef UNINSTALLING"
  for _p in "$PKGDIR"/pedals/*.vst3; do
    [ -d "$_p" ] || continue
    _n="$(basename "$_p" .vst3)"
    printf '    ${If} ${FileExists} "$Vst3Dir\\%s.vst3\\Contents\\x86_64-win\\%s.vst3"\n' "$_n" "$_n"
    printf '        RMDir /r "$Vst3Dir\\%s.vst3"\n' "$_n"
    printf '        DetailPrint "Removed $Vst3Dir\\%s.vst3"\n' "$_n"
    printf '    ${EndIf}\n'
  done
  echo "!else"
  for _p in "$PKGDIR"/pedals/*.vst3; do
    [ -d "$_p" ] || continue
    _n="$(basename "$_p" .vst3)"
    printf '    ${If} ${FileExists} "$Vst3Dir\\%s.vst3\\Contents\\*.*"\n' "$_n"
    printf '        RMDir /r "$Vst3Dir\\%s.vst3"\n' "$_n"
    printf '    ${EndIf}\n'
    printf '    SetOutPath "$Vst3Dir\\%s.vst3"\n' "$_n"
    printf '    File /r "${PEDALS_DIR}\\%s.vst3\\*"\n' "$_n"
    printf '    DetailPrint "Installed $Vst3Dir\\%s.vst3"\n' "$_n"
  done
  echo "!endif"
} > "$PEDALS_NSH"

PEDAL_N="$(grep -c 'SetOutPath' "$PEDALS_NSH" || true)"
[ "$PEDAL_N" = "5" ] ||
  namp_dist_die "the generated installer list holds $PEDAL_N pedals, expected 5. An installer that
puts four of five on disk looks like it worked."

# --- the notes ---------------------------------------------------------------
# The licence paragraph differs between the two configurations and says so plainly, because "this
# program is MIT" printed on a GPLv3 binary is the kind of wrong that a user acts on.
if [ "$WANT_ASIO" = "ON" ]; then
  LICENCE_PARA="Almost all of this is MIT - see LICENSE, and NOTICE for third-party
attribution. ONE file in this archive is not, and it is worth knowing which:

    rack\\namp-rack.exe    GNU GPL version 3

The standalone hosts ASIO, Steinberg's SDK for that is dual-licensed, and this
project takes its GPLv3 arm rather than sign a proprietary agreement. So you get
that one program under GPLv3 and you are entitled to its complete source.
COPYING.GPL-3 has the terms, CORRESPONDING-SOURCE.txt says where the source is,
and LICENSE-windows-asio.txt explains the whole arrangement in one page.

Everything else here - the plug-in, the five pedals, and every line of this
project's own source - is MIT and stays MIT. They are separate programs that
happen to be in the same download, which is what the GPL calls an aggregate, and
being next to a GPLv3 binary does not change their terms. The Linux release,
which has no ASIO in it, is MIT throughout."
else
  LICENCE_PARA="MIT, all of it. See LICENSE, and NOTICE for third-party attribution.

This build has no ASIO in it, which is what keeps the standalone MIT - the
Windows standalone WITH ASIO is GPLv3 instead."
fi

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp ${VERSION} - a four-channel Neural Amp Modeler amp head, for 64-bit Windows

One amp, three ways to run it, and one installer that installs all of them.

    plugin\\NAMp-rations.vst3   the amp as a plug-in, for your DAW
    rack\\namp-rack.exe         the same amp standalone, in its own window, with
                               a rack that hosts other people's VST3 plug-ins
                               before and after it
    pedals\\                    five pedals, as ordinary plug-ins

The plug-in and the standalone are the SAME amp from the SAME source, released
together and versioned together. The difference is the surround: the plug-in has
a five-pedal pedalboard built into its panel, and the standalone leaves that out
because it hosts your own plug-ins instead - including the five in pedals\\, which
reach it by exactly the route everybody else's plug-in takes.

Install, the easy way
---------------------
Run NAMp-install.exe. It asks which of the three you want and installs them.

It is not code-signed, so Windows SmartScreen will show a blue "Windows protected
your PC" box. Click "More info", then "Run anyway" - or install by hand instead,
below; the two put exactly the same folders in exactly the same places.

Run it as an administrator and it installs for everyone, in
C:\\Program Files\\Common Files\\VST3 and C:\\Program Files\\NAMp. Run it normally
and it installs just for you, in %LOCALAPPDATA%\\Programs\\Common\\VST3 and
%LOCALAPPDATA%\\Programs\\NAMp. Either way it tells you which, and you can change
the VST3 folder. To remove it later, use "Apps & features" in Windows Settings,
or the uninstaller it leaves beside the program.

Install, by hand
----------------
A VST3 plug-in is installed by copying its folder to where hosts look. There are
two such places, and hosts search them in this order:

  1. Just for you - no administrator rights needed:

         %LOCALAPPDATA%\\Programs\\Common\\VST3\\

     Paste that into the Explorer address bar. If the VST3 folder is not there,
     create it.

  2. For every user on the machine - needs administrator rights:

         C:\\Program Files\\Common Files\\VST3\\

Copy plugin\\NAMp-rations.vst3 into one of them, and the five folders in pedals\\
as well if you want them, then rescan plug-ins in your DAW.

Do not rename anything inside a bundle. Each carries its own art and fonts in
Contents\\Resources, and the binary in Contents\\x86_64-win must keep the bundle's
own name or no host will load it.

The standalone needs no installation at all. rack\\namp-rack.exe is the whole
program - the amp, its art and its fonts are inside it - so put it wherever you
like and run it.

Whichever way you install, make sure you only have ONE copy of each bundle. Hosts
scan both folders, so a copy left in each shows up twice in the plug-in list. The
installer offers to remove the other one for you.

Captures
--------
NAMp ships NO captures - it plays yours, and it wants four sets of them.

In the plug-in, click "Captures, MIDI, Settings", top right. In the standalone,
open the setup page. Either way, point each channel's loader at a FOLDER of .nam
files, or at a single .nam. That folder's name becomes the channel's name on the
front panel, and you can type over it if you would rather call it something else.

Each channel's dial then sweeps that whole bank continuously - no reload, no
click, no dialog. One click of the mouse wheel on a channel dial is exactly one
capture. Rest on one and you are playing that capture exactly; in between, you
are hearing the two either side blended.

Captures are ordered by the number in the filename (the LAST run of digits, so
"GAIN 2" comes before "GAIN 10"), then anything ending in MAX, then anything with
no number at all, alphabetically. So capture your amp at each mark of its own gain
control and put that mark last in the name:

    MyAmp - crunch - GAIN 1.nam
    MyAmp - crunch - GAIN 2.nam
    ...
    MyAmp - crunch - GAIN MAX.nam

and that channel's dial is that amp's gain control, at the amp's own spacing.

A channel with nothing loaded is silent rather than broken; a fresh instance has
four of them. Captures must be feed-forward (WaveNet or ConvNet); an LSTM capture
is refused rather than silently accepted.

The four channels
-----------------
Exactly one channel sounds at a time. Click its bat switch, or learn a MIDI
footswitch to it, and the change is instant and silent even mid-note - which is
the whole reason this project exists.

The settings page also carries a trim per channel, the MIDI learn rows, and the
output section: Raw / Normalized / Calibrated, plus input calibration. Normalized
is the default.

MIDI learn: a learned CC or Program Change answers on ANY MIDI channel, because
both arrive at a VST3 plug-in as parameter changes and the channel is already gone
by then. Only a learned NOTE can be pinned to one MIDI channel.

The rest of the panel
---------------------
Shared Threshold / Bass / Middle / Treble, Input and Output, each reading its
value under the dial. BYPASS, EQ and GATE switch out the whole chain, the tone
stack and the noise gate. The icon beside the settings button is Slim, which
trades model size for CPU - it appears only when your captures can actually use
it. A cabinet section loads one or two impulse responses with a blend between
them.

The file picker is drawn inside the panel rather than being a Windows dialog, so
it looks like the rest of the program. ".." goes up; above a drive root it lists
the drives, so captures on D: are reachable.

The five pedals
---------------
    RationsBoost      a Tube Screamer-style overdrive
    RationsChorus     two modulated taps per channel
    RationsFlanger    swept comb with feedback
    RationsDelay      tempo-syncable, optional ping-pong
    RationsReverb     a Freeverb-lineage room

The installer puts them in your VST3 folder, so they turn up in the standalone's
plug-in list after a rescan, and in your DAW's as well. They are also built into
the plug-in's own pedalboard page, so you do not need them installed to use that -
the separate bundles are for the standalone and for any other host.

The standalone
--------------
It opens ASIO first, because that is what your interface's own driver exposes and
what the latency depends on, and falls back to WASAPI when the machine has no ASIO
driver installed. --list-devices shows what it can see.

If you have an audio interface, install its manufacturer's ASIO driver and use
that. ASIO4ALL is a wrapper rather than a driver and will work, but the latency is
not what a real driver gives you.

It hosts VST3. It does NOT host LV2 on Windows - lilv and suil are Linux libraries
and there is no Windows build of this program that includes them, so LV2 plug-ins
will not appear in the list and that is by design rather than a missing dependency.

Plug-ins are scanned in a separate process on purpose: one that crashes while it is
being examined takes that process with it and is recorded as bad rather than
retried, so a single broken plug-in on your disk cannot stop the rack starting.
Adding and removing plug-ins while audio is running is safe and is not heard.

Saved racks, which captures were loaded, your audio device choice and your MIDI
bindings live under %APPDATA%\\NAMp-Rack. The plug-in scan cache lives under
%LOCALAPPDATA%\\NAMp-Rack and can be deleted at any time; a rescan rebuilds it.

Requirements
------------
64-bit Windows. Nothing else: cairo, FreeType, libpng and zlib are statically
linked, and so is the C++ runtime. There is no redistributable to install and
nothing to put beside any of the binaries.

There is no 32-bit build. The Linux release is a separate download and holds the
same three things, plus an LV2 build of the plug-in.

Licence
-------
${LICENCE_PARA}

NOTICE has the third-party attribution, and it matters more for this build than
for the Linux one: these binaries statically link cairo, pixman, FreeType, libpng
and zlib and therefore redistribute them.

The five pedals carry their own attribution in pedals\\NOTICE, covering components
they use and this project does not.

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe
and other countries.
EOF

MARK=""
if [ -n "$NOSOURCE" ]; then
  MARK="-NOSOURCE"
  echo
  echo "WARNING: this archive contains a GPLv3 binary whose Corresponding Source was" >&2
  echo "not assembled, because $NOSOURCE." >&2
  echo "It is named -NOSOURCE and MUST NOT be published -- GPLv3 section 6 requires the" >&2
  echo "source to be available to whoever receives the binary. The binaries themselves are" >&2
  echo "fine and every check above passed; commit the tree and re-run to get a releasable one." >&2
  echo
fi

# THE LICENCE FILES ARE ASSERTED, NOT ASSUMED. GPLv3 section 4 requires the licence text to travel
# with the program and section 6(d) requires the directions to the source to sit next to the object
# code. Both are one `cp` away from being silently dropped by an edit to the block above, and
# neither absence would break anything a user would notice -- which is exactly the shape of mistake
# a gate is for.
if [ "$WANT_ASIO" = "ON" ]; then
  namp_dist_require_files "$PKGDIR" "the GPLv3 archive" \
    COPYING.GPL-3 LICENSE-windows-asio.txt NOTICE LICENSE
  if [ -z "$NOSOURCE" ]; then
    namp_dist_require_files "$PKGDIR" "the GPLv3 archive" CORRESPONDING-SOURCE.txt
    [ -f "$CS_TARBALL" ] ||
      namp_dist_die "the Corresponding Source tarball was not produced at $CS_TARBALL. A GPLv3
binary may not be published without it."
  else
    namp_dist_require_files "$PKGDIR" "the unpublishable archive" NOT-A-RELEASE.txt
  fi
  # The GPL text must be the real one. A truncated or reflowed copy is not the licence, and this
  # file is copied around by hand often enough to be worth checking rather than trusting.
  grep -qx '                       Version 3, 29 June 2007' "$PKGDIR/COPYING.GPL-3" ||
    namp_dist_die "COPYING.GPL-3 in the archive is not the verbatim GPLv3 text."
fi

# --- the installer ------------------------------------------------------------
# Built LAST, from the staged tree, so NAMp-install.exe carries exactly the binaries that are also
# loose in the ZIP - the same stripped, de-randomised files and the same moduleinfo.json. Building
# it from the build directories instead would quietly ship unstripped, unverified copies the moment
# any step above changed.
if [ "${NAMP_SKIP_INSTALLER:-0}" = "1" ]; then
  echo
  echo "WARNING: NAMP_SKIP_INSTALLER=1 - the ZIP has no NAMp-install.exe." >&2
  echo
else
  # VIProductVersion wants exactly four components; project() gives three.
  VERSION4="$VERSION"
  while [ "$(printf '%s' "$VERSION4" | tr -cd '.' | wc -c)" -lt 3 ]; do
    VERSION4="$VERSION4.0"
  done

  # TWO DEFINES, BECAUSE THEY ARE CONDITIONAL ON DIFFERENT THINGS. GPLV3_DOCS says the standalone
  # contains GPLv3 code, so section 4's copy of the licence must accompany it -- true of every ASIO
  # build including one whose source was not assembled. CS_DOC says the 6(d) directions exist to be
  # installed, which is only true when corresponding-source.sh actually ran. Collapsing the two
  # into one condition is what the first version of this did, and it silently left an ASIO binary
  # on disk with no copy of the licence that covers it.
  NSIS_GPL=()
  [ "$WANT_ASIO" = "ON" ] && NSIS_GPL+=("-DGPLV3_DOCS=1")
  [ -f "$CS_DIRECTIONS" ] && NSIS_GPL+=("-DCS_DOC=1")

  # STAMP BEFORE BUILDING THE INSTALLER, NOT ONLY BEFORE ZIPPING. NSIS's SetDateSave defaults ON,
  # so makensis records each staged file's mtime inside NAMp-install.exe and restores it on
  # install. Those mtimes are whatever the staging `cp` happened to write, so without this the
  # installer moved on every run even though every binary it carries is byte-identical -- and it
  # would have gone on doing so after the ZIP itself was cured, because the ZIP is built later and
  # from the same tree. Stamping here fixes both, and gives the installed files a date that is a
  # fact about the release rather than about when someone packaged it.
  namp_dist_stamp_tree "$PKGDIR" "$(namp_dist_epoch "$REPO")"

  echo "building NAMp-install.exe with $MAKENSIS"
  "$MAKENSIS" -V2 -NOCD \
    "-DVERSION=$VERSION" "-DVERSION4=$VERSION4" \
    "-DPLUGIN_DIR=$PKGDIR/plugin/NAMp-rations.vst3" \
    "-DPEDALS_DIR=$PKGDIR/pedals" \
    "-DRACK_EXE=$PKGDIR/rack/namp-rack.exe" \
    "-DDOC_DIR=$PKGDIR" \
    "-DPEDALS_NSH=$PEDALS_NSH" \
    "${NSIS_GPL[@]+"${NSIS_GPL[@]}"}" \
    "-DOUTFILE=$PKGDIR/NAMp-install.exe" \
    "$REPO/installer/namp.nsi"

  [ -s "$PKGDIR/NAMp-install.exe" ] || namp_dist_die "makensis produced no NAMp-install.exe"
  # It must be a PE executable, not whatever else ended up at that path. file(1) is not guaranteed
  # to be installed, so check the magic directly.
  [ "$(head -c2 "$PKGDIR/NAMp-install.exe")" = "MZ" ] ||
    namp_dist_die "NAMp-install.exe is not a PE executable"

  # THE INSTALLER IS AN ARTEFACT LIKE ANY OTHER and carries the same wall-clock stamp everything
  # else here had to be cured of. NSIS writes a COFF TimeDateStamp into the stub it links, so two
  # release runs from one commit produced two different installers even after every binary inside
  # them had been made reproducible.
  namp_dist_pe_derandomise "$PKGDIR/NAMp-install.exe"
  namp_dist_pe_assert_no_timestamp "$PKGDIR/NAMp-install.exe" "NAMp-install.exe"
fi

PKGNAME="NAMp-${VERSION}${MARK}-windows-x86_64"
mv "$PKGDIR" "$STAGEDIR/$PKGNAME"
# The epoch makes the ZIP reproducible, not just the binaries in it -- see namp_dist_zip.
namp_dist_zip "$STAGEDIR" "$PKGNAME" "$REPO/dist/${PKGNAME}.zip" "$(namp_dist_epoch "$REPO")"
