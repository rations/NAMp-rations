// FileBrowser implementation. See filebrowser.h.

#include "filebrowser.h"
#include "gfx/palette.h"
#include "platform/respath.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace Rations
{

namespace fs = std::filesystem;

namespace
{

// Everything is measured from wherever setBounds() put the card, so nothing
// here knows about the editor's layout — which is what lets the card be sized
// against the cabinet page's window rather than the head's.
constexpr float kHeaderH = 52.0f;
constexpr float kRowH = 24.0f;
constexpr float kRowIndent = 14.0f;

// Card colours, a shade off the faceplate so the overlay reads as floating.
constexpr uint32_t kCardBg = 0x161314;
constexpr uint32_t kCardBorder = 0x3A3634;
constexpr uint32_t kRowBgOdd = 0x1C1819;
constexpr uint32_t kRowBgEven = 0x191516;

// `ext` is one extension ("nam") or a comma-separated list of them
// ("vst3,lv2"), because one of the two callers is looking for plug-ins and a
// folder of plug-ins holds more than one kind. Comparison is case-insensitive
// and the leading dot is never included on either side.
bool endsWithExtension(const std::string &name, const std::string &ext)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
        return false;
    const char *suffix = name.c_str() + dot + 1;

    size_t start = 0;
    while (start <= ext.size()) {
        const size_t comma = ext.find(',', start);
        const std::string one =
            ext.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!one.empty() && strcasecmp(suffix, one.c_str()) == 0)
            return true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}

// Where to start when the caller has no usable path of its own.
fs::path homeDirectory()
{
#if defined(_WIN32)
    // USERPROFILE, not HOME: HOME is a POSIX convention that Windows only sets
    // under a Unix-ish shell, so relying on it would land somewhere arbitrary
    // or nowhere. With no profile at all, the drive list is the honest answer —
    // there is no single filesystem root to fall back to.
    if (const char *profile = std::getenv("USERPROFILE"))
        return fs::path(profile);
    return fs::path();
#else
    if (const char *home = std::getenv("HOME"))
        return fs::path(home);
    return fs::path("/");
#endif
}

#if !defined(_WIN32)
// True when a path is the top of the tree it is in — on POSIX, "/" and nothing
// else.
//
// has_parent_path() cannot be used for this. Verified by running it: on Windows
// fs::path("C:\\").has_parent_path() is TRUE and its parent_path() is "C:\"
// again, so a ".." row gated on it would exist and go nowhere. Comparing the
// parent to the path itself is true at "/" and at every spelling of a drive
// root ("C:\", "C:/", "C:"), so the same test would serve on both platforms —
// but on Windows a drive root is NOT the top (the drive list is above it), so
// only the POSIX side has a use for it. See atTop().
bool isRoot(const fs::path &p)
{
    return p.parent_path() == p;
}
#endif

} // namespace

//------------------------------------------------------------------------
void FileBrowser::setDirectoryChoice(std::string noun, bool countBundles, bool requireMatches)
{
    mNoun = std::move(noun);
    mCountBundles = countBundles;
    mRequireMatches = requireMatches;
}

//------------------------------------------------------------------------
void FileBrowser::open(const std::string &startPath, const std::string &extension,
                       const std::string &title, Mode mode)
{
    mMode = mode;
    mExt = extension;
    mTitle = title;
    mChosen.clear();
    mChosenIsDir = false;
    mScroll = 0;

    // startPath comes from the plug-in's state, so it is untrusted: on Windows a
    // string that is not valid UTF-8 cannot be made into a path at all and would
    // throw out of here into the host's message loop (platform/respath.h). A
    // path we cannot express is treated as no path, which lands on the home
    // directory — the same as never having chosen one.
    std::error_code ec;
    fs::path start;
    if (!utf8ToPath(startPath, start) || startPath.empty() || !fs::exists(start, ec)) {
        start = homeDirectory();
    } else if (!fs::is_directory(start, ec)) {
        start = start.parent_path();
    }

    mDir = start.string();
    listDirectory();
    mOpen = true;
}

void FileBrowser::close()
{
    mOpen = false;
    mEntries.clear();
    mFileCount = 0;
}

//------------------------------------------------------------------------
void FileBrowser::listDirectory()
{
    mEntries.clear();
    mFileCount = 0;
    mScroll = 0;

    if (mDir.empty()) {
        listDrives(); // Windows only; on POSIX mDir is never empty
        return;
    }

    std::vector<Entry> dirs, files;
    std::error_code ec;

    // A directory we cannot read (permissions, a stale path) must not throw out
    // of the run loop: the iterator is constructed with an error_code and
    // simply yields nothing, leaving a browsable ".." row.
    fs::directory_iterator it(mDir, fs::directory_options::skip_permission_denied, ec);
    if (!ec) {
        for (const fs::directory_entry &entry : it) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.')
                continue; // hidden files, as most pickers default to

            std::error_code dirEc;
            if (entry.is_directory(dirEc)) {
                dirs.push_back({name, true});
                // A .vst3 or a .lv2 is a directory, so a folder full of plug-ins
                // holds zero matching FILES and the footer would report nothing
                // at all. Counted here rather than guessed at from the extension.
                if (mCountBundles && endsWithExtension(name, mExt))
                    ++mFileCount;
            } else if (endsWithExtension(name, mExt)) {
                files.push_back({name, false});
            }
        }
    }
    mFileCount += static_cast<int>(files.size());

    auto byName = [](const Entry &a, const Entry &b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    if (!atTop())
        mEntries.push_back({"..", true});
    mEntries.insert(mEntries.end(), dirs.begin(), dirs.end());
    // In directory mode the files are listed but inert — they are shown so the
    // folder's contents are visible before committing to it.
    mEntries.insert(mEntries.end(), files.begin(), files.end());
}

//------------------------------------------------------------------------
// Windows has no single filesystem root, so the level above a drive root is a
// synthetic one that lists the drives themselves — otherwise a capture folder
// on D: would be unreachable, there being no text field to type a path into.
// An empty mDir IS that level: the empty string is not a path Windows can
// express, so it cannot collide with a real directory.
//
// On POSIX this never runs. "/" is the top and mDir is never empty there.
void FileBrowser::listDrives()
{
#if defined(_WIN32)
    // A bitmask, bit 0 = A:. Drives that exist but are not ready (an empty
    // optical drive, a dropped network mapping) are included; entering one just
    // yields an empty listing with a ".." row, which is a state to back out of
    // rather than a failure.
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i))
            mEntries.push_back({std::string(1, static_cast<char>('A' + i)) + ":", true});
#endif
}

//------------------------------------------------------------------------
// Is there anywhere above the current directory? A drive root's parent is the
// drive list, so on Windows the top is the synthetic level, not the root.
bool FileBrowser::atTop() const
{
#if defined(_WIN32)
    return mDir.empty();
#else
    return isRoot(fs::path(mDir));
#endif
}

//------------------------------------------------------------------------
// Where clicking the directory row `e` leads. In one place because two of the
// moves are not plain joins: ".." at a drive root leaves for the drive list,
// and a drive row leaves it again.
std::string FileBrowser::targetOf(const Entry &e) const
{
    if (mDir.empty()) {
        // "C:" on its own is DRIVE-RELATIVE — it means the current directory on
        // C:, not its root — so the separator is not decoration here.
        return e.name + "\\";
    }

    if (e.name != "..")
        return (fs::path(mDir) / e.name).string();

    const fs::path here(mDir);
    const fs::path up = here.parent_path();
#if defined(_WIN32)
    if (up == here)
        return std::string(); // out of the drive, into the drive list
#endif
    return up.string();
}

//------------------------------------------------------------------------
// One rule for "the current folder is a valid answer", so the footer's enabled
// look and the click that acts on it cannot disagree.
bool FileBrowser::canChooseCurrent() const
{
    if (mDir.empty())
        return false; // the drive list is not a folder
    return mFileCount > 0 || !mRequireMatches;
}

//------------------------------------------------------------------------
// Every mode that can choose a folder needs a footer for the "use this folder"
// button; pure file mode chooses by clicking a row and needs only a small
// bottom margin.
bool FileBrowser::offersDirectory() const
{
    return mMode == Mode::Directory || mMode == Mode::FileOrDirectory;
}

float FileBrowser::footerHeight() const
{
    return offersDirectory() ? 44.0f : 10.0f;
}

float FileBrowser::listTop() const
{
    return mBounds.y + kHeaderH;
}

float FileBrowser::listHeight() const
{
    return mBounds.h - kHeaderH - footerHeight();
}

Rect FileBrowser::chooseButton() const
{
    const float h = 26.0f;
    return Rect(mBounds.right() - 190.0f, mBounds.bottom() - footerHeight() + 8.0f, 176.0f, h);
}

// Close cross, top-right of the card.
Rect FileBrowser::closeBox() const
{
    return Rect(mBounds.right() - 30.0f, mBounds.y + 14.0f, 16.0f, 16.0f);
}

int FileBrowser::visibleRows() const
{
    return static_cast<int>(listHeight() / kRowH);
}

int FileBrowser::rowAt(float x, float y) const
{
    if (x < mBounds.x || x >= mBounds.right() || y < listTop() || y >= listTop() + listHeight())
        return -1;
    const int row = static_cast<int>((y - listTop()) / kRowH) + mScroll;
    return (row >= 0 && row < static_cast<int>(mEntries.size())) ? row : -1;
}

//------------------------------------------------------------------------
void FileBrowser::draw(Canvas &c)
{
    if (!mOpen)
        return;

    // Dim the panel behind, then the overlay card.
    c.setColor(0x000000, 170);
    c.fillRect(c.bounds());

    const Rect panel = mBounds;
    c.setColor(kCardBg);
    c.fillRoundRect(panel, 10.0f);
    c.setColor(kCardBorder);
    c.setPenSize(1.0f);
    c.strokeRoundRect(panel, 10.0f);

    // --- header: title, current directory, close ---
    c.setFont(Font::Title);
    c.setFontSize(14);
    c.setColor(pal::kTextColor);
    c.drawString(mTitle.c_str(), mBounds.x + kRowIndent, mBounds.y + 26.0f);
    c.setFont(Font::Body);

    c.setFontSize(11);
    c.setColor(pal::kDimColor);
    {
        // An empty mDir is the Windows drive list, which has no path to show.
        const std::string where = mDir.empty() ? std::string("This PC") : mDir;
        const std::string dir = c.clipToWidth(where, mBounds.w - 2 * kRowIndent - 30.0f);
        c.drawString(dir.c_str(), mBounds.x + kRowIndent, mBounds.y + 44.0f);
    }

    c.setColor(pal::kDimColor);
    c.setPenSize(2.0f);
    c.strokeLine(closeBox().left(), closeBox().top(), closeBox().right(), closeBox().bottom());
    c.strokeLine(closeBox().left(), closeBox().bottom(), closeBox().right(), closeBox().top());
    c.setPenSize(1.0f);

    // --- list ---
    const float top = listTop(), listH = listHeight();
    c.pushClip(Rect(mBounds.x, top, mBounds.w, listH));
    c.setFontSize(12);
    const int rows = visibleRows();
    for (int i = 0; i < rows; ++i) {
        const int index = mScroll + i;
        if (index >= static_cast<int>(mEntries.size()))
            break;
        const Entry &e = mEntries[static_cast<size_t>(index)];
        const float y = top + i * kRowH;

        c.setColor(index % 2 ? kRowBgOdd : kRowBgEven);
        c.fillRect(Rect(mBounds.x + 1.0f, y, mBounds.w - 2.0f, kRowH));

        // A file in pure directory mode is context, not a choice, so it is
        // drawn in the disabled colour and clicking it does nothing. In
        // FileOrDirectory it is a choice and reads like one — that grey is what
        // said a single capture could not be loaded.
        const bool inert = (mMode == Mode::Directory && !e.isDir);
        c.setColor(e.isDir ? pal::kAccent : (inert ? 0x6A6460 : pal::kTextColor));
        const std::string label = e.isDir ? (e.name + "/") : e.name;
        c.drawString(c.clipToWidth(label, mBounds.w - 2 * kRowIndent).c_str(),
                     mBounds.x + kRowIndent, y + kRowH - 7.0f);
    }
    if (mEntries.empty()) {
        c.setColor(pal::kDimColor);
        const std::string msg = "Nothing here";
        c.drawString(msg.c_str(), mBounds.x + kRowIndent, top + kRowH);
    }
    c.popClip();

    // --- scroll indicator, only when the list overflows ---
    if (static_cast<int>(mEntries.size()) > rows) {
        const float trackX = mBounds.right() - 6.0f;
        const float frac = static_cast<float>(rows) / static_cast<float>(mEntries.size());
        const float thumbH = std::max(listH * frac, 20.0f);
        const float maxScroll = static_cast<float>(static_cast<int>(mEntries.size()) - rows);
        const float pos = maxScroll > 0 ? static_cast<float>(mScroll) / maxScroll : 0.0f;
        c.setColor(kCardBorder);
        c.fillRect(Rect(trackX, top, 3.0f, listH));
        c.setColor(pal::kAccent);
        c.fillRect(Rect(trackX, top + pos * (listH - thumbH), 3.0f, thumbH));
    }

    // --- footer: choose-this-directory button ---
    if (offersDirectory()) {
        // When a file is choosable too, the footer says so. Nothing else on the
        // card distinguishes the two answers, and a list of live rows beside a
        // "Use this folder" button is exactly the place someone would assume
        // the folder is the only answer.
        char count[160];
        if (mMode == Mode::FileOrDirectory && mFileCount > 0)
            snprintf(count, sizeof(count), "%d %s%s here  ·  click one to load it on its own",
                     mFileCount, mNoun.c_str(), mFileCount == 1 ? "" : "s");
        else
            snprintf(count, sizeof(count), "%d %s%s in this folder", mFileCount, mNoun.c_str(),
                     mFileCount == 1 ? "" : "s");
        c.setFontSize(11);
        c.setColor(mFileCount > 0 ? pal::kDimColor : 0x6A6460);
        c.drawString(count, mBounds.x + kRowIndent, mBounds.bottom() - 20.0f);

        // Loading a capture folder with no captures is allowed to be refused
        // here rather than failing later in the worker: it is the one mistake
        // the browser can see coming. A search folder is different — an empty
        // one is a perfectly reasonable thing to add before installing into it —
        // so the caller says which rule applies.
        const Rect button = chooseButton();
        const bool enabled = canChooseCurrent();
        c.setColor(enabled ? pal::kAccent : 0x2A2624, enabled ? 60 : 255);
        c.fillRoundRect(button, 5.0f);
        c.setColor(enabled ? pal::kAccent : 0x555049);
        c.setPenSize(1.0f);
        c.strokeRoundRect(button, 5.0f);
        c.setFontSize(12);
        c.setColor(enabled ? pal::kTextColor : 0x6A6460);
        const char *label = "Use this folder";
        c.drawString(label, button.centerX() - c.stringWidth(label) * 0.5f, button.bottom() - 9.0f);
    }
}

//------------------------------------------------------------------------
FileBrowser::Result FileBrowser::handleClick(float x, float y)
{
    if (!mOpen)
        return Result::NoResult;

    // Clicking outside the card, or on the close cross, dismisses.
    const Rect panel = mBounds;
    if (!panel.contains(x, y) || closeBox().inset(-6.0f).contains(x, y)) {
        close();
        return Result::Cancelled;
    }

    if (offersDirectory() && chooseButton().contains(x, y)) {
        if (!canChooseCurrent())
            return Result::Handled; // nothing to load; the button reads disabled
        mChosen = mDir;
        mChosenIsDir = true;
        close();
        return Result::Chosen;
    }

    const int row = rowAt(x, y);
    if (row < 0)
        return Result::Handled; // inside the card but on no row: swallow it

    const Entry &e = mEntries[static_cast<size_t>(row)];
    if (e.isDir) {
        std::error_code ec;
        const std::string next = targetOf(e);
        // The drive list is not a directory anything can stat, so it is reached
        // by being empty rather than by passing the is_directory test.
        if (next.empty() || fs::is_directory(fs::path(next), ec)) {
            mDir = next;
            listDirectory();
        }
        return Result::Handled;
    }

    if (mMode == Mode::Directory)
        return Result::Handled; // files are context here, not choices

    mChosen = (fs::path(mDir) / e.name).string();
    mChosenIsDir = false;
    close();
    return Result::Chosen;
}

//------------------------------------------------------------------------
bool FileBrowser::handleWheel(int delta)
{
    if (!mOpen)
        return false;
    const int rows = visibleRows();
    const int maxScroll = std::max(0, static_cast<int>(mEntries.size()) - rows);
    const int next = std::min(std::max(mScroll - delta * 3, 0), maxScroll);
    if (next == mScroll)
        return false;
    mScroll = next;
    return true;
}

} // namespace Rations
