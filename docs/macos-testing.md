# Testing NAMp Rations on macOS

This is for whoever has a Mac. It is written out in full rather than left to
judgement, because **nothing in this checklist can be automated and none of it
has ever been done** — every macOS build so far has come off a hosted build
runner where the editor is rendered and audited page by page and the SDK's
validator passes, and where nobody can hear it.

You do not need to run any tests, install any tools, or build anything. You need
a DAW, your own capture files, and about half an hour. **Everything below is a
thing you look at or listen to.**

---

## What is already known, so you do not report it as a bug

Three things are deliberate and are not worth reporting:

- **The plug-in is not notarized by Apple.** It is "ad-hoc signed", which is
  enough to run but not enough for Gatekeeper. `install.sh` clears the
  quarantine flag for you; if you copy the bundle by hand instead you have to run
  `xattr -cr` on it yourself. INSTALL.txt in the archive says so.
- **There is no standalone application.** The Linux release has one because it is
  a JACK client. The macOS release is the plug-in only.
- **The DSP has never been measured on macOS.** It is measured on Linux and on
  Windows, to identical figures on both, but the proofs that produce those
  numbers need capture files that are not ours to distribute, so they cannot run
  on a build runner. Your ears are the only check that exists. That is exactly
  why "it sounds wrong" is worth reporting even if you cannot say why.

---

## 1. Install

This is the **only** test the ad-hoc signature ever gets, so do it on a machine
that has never had this plug-in on it.

**You will need Terminal once, for about thirty seconds, and you do not have to
type any file names.** Only once, and only to install — everything after this is
your DAW and your ears. The reason it cannot be done entirely in Finder is the
quarantine flag explained above.

- [ ] Download the archive and **double-click it in Finder** to unpack it (not
      from a terminal — the quarantine flag is set by the download, and unpacking
      it the ordinary way is what a real user does, so it is what needs testing).
      A folder appears beside the .zip.
- [ ] Open Terminal: hold **Command** and press the **space bar**, type
      `terminal`, press **Return**. A window with a text prompt opens.
- [ ] In that window type the four letters `bash` and then **one space**. Do not
      press Return yet.
- [ ] **Drag the file `install.sh`** out of the folder and drop it onto the
      Terminal window. Its location appears after what you typed.
- [ ] Press **Return**. It should print `Installed:` and a location.

If it prints anything else, that is a result — copy the whole window and send it.

- [ ] Open your DAW, rescan plug-ins, and confirm **NAMp Rations** appears.
- [ ] Add it to a track. It should open showing an amp head with four dials and
      four empty channel names.

If the DAW finds it and then refuses to load it, that is the quarantine flag and
the interesting question is whether `install.sh` failed to clear it. Say which
DAW, and paste whatever it said.

- [ ] Removing it again works: the same five steps, but before pressing Return
      also type one space and then `--uninstall`. After a rescan the DAW should
      no longer list it.

## 2. Load your captures

A fresh instance has four empty channels; this is the one thing a new user has
to do, and on macOS it is also the only exercise the capture loader, the bank
worker and the model builder ever get.

- [ ] Click **Captures, MIDI, Settings**, top right.
- [ ] Point each of the four loaders at a folder of `.nam` files (or at a single
      one).
- [ ] The channel takes the folder's name. Confirm that name appears above the
      matching dial on the head page.
- [ ] Save the project, close it, reopen it. The four banks should come back.

## 3. Play it

- [ ] Each of the four channels makes a sound, and it is the sound you expect
      from those captures.
- [ ] Sweep a channel's gain dial across its whole bank. It should move smoothly
      between captures with **no clicks, crackles or gaps** at any point.
- [ ] Switch channels with the bat switches while playing a sustained note. The
      change should be immediate and **silent** — no click, no gap, no fade you
      can hear. This is the single most important thing in the plug-in.
- [ ] Threshold, Bass, Middle, Treble, Input and Output all do what they say.
- [ ] BYPASS, EQ and GATE each switch their stage out and back in without a
      click.
- [ ] The cabinet page: load one impulse response, then two, and move the blend.
      One IR alone should sound the same whatever the blend knob says.
- [ ] The pedalboard: engage each of the five pedals in turn and confirm it does
      what its name says. Stomping one on or off should not click.

**If you have both an Apple Silicon and an Intel Mac, do this on both.** They are
two different binaries joined into one file, and only one of them runs on any
given machine.

## 4. The footswitch, with the editor closed

This is the thing the plug-in exists for and it has never had any check other
than a person with a pedal.

- [ ] On the settings page, learn a footswitch button to each of the four
      channels (press **Learn** on a row, then stamp the pedal).
- [ ] Learn one to a pedal as well.
- [ ] **Close the editor window** and play. The footswitch must still change
      channels, and the pedal switch must still toggle.
- [ ] Reopen the editor. The bat switch and the pedal's lamp should show what the
      footswitch actually did — if the sound changed but the panel did not, say
      so, because that is a specific known class of bug.
- [ ] Press one button three times. A channel button should select that channel
      every time; a pedal button should go on, off, on.

## 5. The editor

- [ ] **Drag a knob and keep dragging past the edge of the plug-in window.** The
      knob must keep following the mouse until you let go. (On Windows this
      needed explicit work; on macOS it should be free, and this confirms it.)
- [ ] Right-click a control: it should return to its default. **Control-click
      should do the same thing**, because that is the Mac gesture for it.
- [ ] All four page buttons work, and the window **changes shape** each time
      without changing apparent size.
- [ ] Resize the window by dragging its corner. The panel should scale with it
      and stay sharp.
- [ ] On the settings page: the scrollbar on the right can be dragged, and the
      mouse wheel scrolls the page rather than moving whichever control happens
      to be under the pointer.
- [ ] The file browser opens, lists your folders, and `..` goes up.
- [ ] **Type a channel name.** Click the name on a settings-page row, clear it
      with Backspace, and type something **with a capital letter and a space** —
      `JCM800 #2` is the exact case that was got wrong once. Press Return, then
      go to the head page and confirm the new name is above the dial.
- [ ] On a Retina screen the panel should look **sharp**, not soft or scaled up.
      That is worth a second's attention: it is the one thing about the macOS
      editor that is genuinely different from the other two platforms.
- [ ] Nothing flickers, tears, or leaves a stale strip when you resize or change
      pages.

## 6. Dropouts

The closest thing macOS has to the live-audio gate the Linux build runs, and it
is a subjective report on purpose.

- [ ] At **your normal buffer size**, play hard and stomp the footswitch
      repeatedly. Any crackle, click or dropout is worth reporting, with the
      buffer size and sample rate.
- [ ] Same again with all five pedals engaged. (On Linux, at 128 frames, the
      pedalboard and the channel switch together do **not** fit — that is a
      recorded, unresolved finding. Whether Apple Silicon has the same problem is
      completely unknown, and this is the only way to find out.)

---

## What to send back

Prose is fine. For anything that went wrong, the useful details are:

- which Mac (Apple Silicon or Intel, and the macOS version),
- which DAW and version,
- sample rate and buffer size,
- what you did and what happened.

If the editor misbehaves, running the DAW from Terminal with
`RATIONS_MAC_TRACE=1` set makes the plug-in print what it thinks the window is
doing. That output is worth having even if it looks like noise.
