// FileBrowser — the modal picker drawn inside the editor.
//
// Browsing in-panel has a hard practical benefit: nothing links a UI toolkit. A
// GTK or Qt dialog inside a plug-in has to share the host's process with
// whatever toolkit the host already initialised, and portal-based pickers drag
// in a D-Bus dependency. Painting the list ourselves through Canvas has neither
// problem, and it looks like the rest of the panel.
//
// This plug-in opens it in two places:
//
//   * Mode::File — pick one file with a given extension. The cabinet page's two
//     IR rows.
//   * Mode::Directory — pick the directory itself, and nothing in it. Matching
//     files are still listed, dimmed and inert, purely so the folder's contents
//     are visible before committing to it; they cost nothing, since the same
//     scan already found them. A footer button chooses the directory shown.
//     Unused here.
//   * Mode::FileOrDirectory — either. chosenIsDirectory() says which happened,
//     so the caller never has to stat the path back. The settings page's four
//     capture rows: a folder of captures is what a channel's dial sweeps and is
//     the normal case, and a single .nam is an ordinary thing to own that plays
//     as a bank of one.
//
// FileOrDirectory had been carried here unused since this file was ported —
// kept because deleting branches out of a port makes every later fix a manual
// merge — for the whole of the time the plug-in shipped its own captures and
// had no loader. It needed no changes when one arrived.
//
// Behaviour otherwise: directories first then files, both alphabetical; a ".."
// row to go up; the mouse wheel scrolls; clicking a directory descends.
// Everything runs on the run-loop thread, and listing one directory is cheap
// enough to do inline on open.

#pragma once

#include "gfx/canvas.h"

#include <string>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class FileBrowser
{
public:
    enum class Mode {
        File,           // choose a file matching the extension
        Directory,      // choose a directory (matching files shown, but not choosable)
        FileOrDirectory // choose either; ask chosenIsDirectory() which it was
    };

    // What a click did, so the panel knows whether to repaint or act.
    enum class Result {
        // NoResult rather than None: <X11/Xlib.h> defines None as a macro, and
        // the editor that owns this browser is built on an X11 view, so this
        // header lands in translation units that include Xlib. A scoped
        // enumerator is no protection against the preprocessor.
        NoResult, // click was outside the browser, or on nothing
        Handled,  // consumed; repaint
        Chosen,   // something was picked; read chosenPath(), browser is closed
        Cancelled // dismissed without choosing
    };

    bool isOpen() const
    {
        return mOpen;
    }
    const std::string &chosenPath() const
    {
        return mChosen;
    }
    // Which kind chosenPath() is. Always false in Mode::File and always true in
    // Mode::Directory; the answer only carries information in
    // Mode::FileOrDirectory, and it is recorded at the moment of the choice
    // rather than derived afterwards — a path can stop being a directory
    // between the click and the question.
    bool chosenIsDirectory() const
    {
        return mChosenIsDir;
    }

    // `extension` is without the dot ("nam", "wav"), or a comma-separated list
    // of them ("vst3,lv2") when more than one kind counts. `startPath` may be a
    // file (its directory is used), a directory, or empty — in which case it
    // falls back to the user's home directory ($HOME, or %USERPROFILE%).
    void open(const std::string &startPath, const std::string &extension, const std::string &title,
              Mode mode);
    void close();

    // Where the card is drawn, in logical units. There is no default: layout
    // belongs to the owner's geometry header (RULES.md section 4), and here
    // that is geometry.h's kBrowser* block, which sizes the card against the
    // cabinet page's own window rather than the head's. Set it before opening;
    // an unset rect draws nothing.
    void setBounds(const Rect &bounds)
    {
        mBounds = bounds;
    }
    const Rect &bounds() const
    {
        return mBounds;
    }

    // For the modes that offer a folder, and all three default to what the
    // capture bank wants. `noun` is what the footer counts. `countBundles` also counts
    // directories whose name ends in the extension, because a .vst3 or a .lv2
    // is a DIRECTORY and would otherwise count as zero of everything.
    // `requireMatches` is what greys the choose button: a capture folder with
    // no captures in it is a mistake worth catching early, whereas a plug-in
    // folder the user is about to add may legitimately be empty today.
    void setDirectoryChoice(std::string noun, bool countBundles, bool requireMatches);

    void draw(Canvas &c);
    Result handleClick(float x, float y);
    bool handleWheel(int delta);
    // Pull the scroll back into range for the bounds the card has NOW. handleWheel already does
    // this for itself, which is enough while the card is a fixed size; it is not enough when the
    // card can be re-bounded under an open list by a window resize, because the next frame is
    // drawn before the next wheel click and would show an empty list.
    void clampScroll();

private:
    struct Entry {
        std::string name;
        bool isDir = false;
    };

    void listDirectory();
    void listDrives();             // the synthetic top level on Windows; see the .cpp
    bool atTop() const;            // is there anywhere above mDir?
    bool canChooseCurrent() const; // is mDir itself a valid answer right now?
    // Where a directory row leads. A path, but returned as a string so this
    // header does not have to pull <filesystem> into everything that draws.
    std::string targetOf(const Entry &e) const;
    bool offersDirectory() const;      // is the folder itself one of the answers?
    int rowAt(float x, float y) const; // index into mEntries, or -1
    int visibleRows() const;
    float footerHeight() const;
    float listTop() const;
    float listHeight() const;
    Rect chooseButton() const;
    Rect closeBox() const;

    bool mOpen = false;
    Mode mMode = Mode::File;
    Rect mBounds;
    std::string mDir;
    std::string mExt;
    std::string mTitle;
    std::string mChosen;
    bool mChosenIsDir = false;
    std::vector<Entry> mEntries;
    int mFileCount = 0; // matching entries in mDir, for the directory-mode footer
    int mScroll = 0;
    std::string mNoun = "capture";
    bool mCountBundles = false;
    bool mRequireMatches = true;
};

} // namespace Rations
