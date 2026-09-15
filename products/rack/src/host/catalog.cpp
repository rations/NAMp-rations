// Catalog implementation. See catalog.h for the scanning strategy and the payload format.

#include "catalog.h"
#if NAMPRACK_HAVE_LV2
#include "lv2world.h"
#endif
#include "backend_vst3.h"
#include "scanchild.h"

#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#if !defined(_WIN32)
extern char **environ;
#endif

namespace NAMp::host
{

namespace
{

// A plug-in that has not answered in this long is not going to. The parent project uses the same
// figure, and it is generous: a cold bundle load is milliseconds, not seconds.
constexpr int kHelperTimeoutMs = 15000;
// A helper that produces more than this is malfunctioning; nothing legitimate approaches it.
constexpr size_t kHelperMaxOutput = 1024u * 1024u;
// Bound on how much of a search root will be walked, so a hostile or accidental deep tree cannot
// turn a scan into a hang.
constexpr int kMaxCandidates = 4096;
constexpr int kMaxWalkDepth = 8;
// Bounds on the identity baseline, which is read back as untrusted input like every other file
// this host reads. A collection of a few thousand plug-ins is already extraordinary.
constexpr size_t kMaxIndexBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxIndexEntries = 100000;
constexpr size_t kMaxIndexLineBytes = 8192;

#if defined(_WIN32)
//------------------------------------------------------------------------
// A UTF-8 string as a wide Win32 one, without ever throwing.
//
// std::filesystem::path is the conversion — libstdc++ treats a path's narrow form as UTF-8
// regardless of the active code page, so this is lossless for anything Windows can name — but
// building one from bytes that are not valid UTF-8 THROWS std::filesystem::filesystem_error, and
// every string that reaches the spawn below came out of a directory walk or an environment
// variable. An unrepresentable name is "no such helper", which the callers already degrade for.
bool utf8ToWide(const std::string &in, std::wstring &out)
{
    try {
        out = std::filesystem::path(in).wstring();
    } catch (...) {
        out.clear();
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
// One argument appended to a Win32 command line, quoted so that the CRT's own parser hands the
// child back exactly the string given here.
//
// CreateProcess takes ONE string and the child splits it again, so this has to be done rather than
// skipped: a plug-in under "C:\Program Files\Common Files\VST3" is the normal case, not the exotic
// one, and an unquoted space would have the child probing "C:\Program". The rule being implemented
// is the documented one CommandLineToArgvW and the Microsoft CRT share — a run of backslashes is
// doubled only when a quote (or the closing quote) follows it, and is literal otherwise.
void appendCommandLineArg(std::wstring &out, const std::wstring &arg)
{
    if (!out.empty())
        out += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\'); // they precede the closing quote
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += *it;
        }
    }
    out += L'"';
}
#endif

//------------------------------------------------------------------------
// This process's own executable, which is what a scan child is spawned from. Read from the
// operating system rather than from argv[0]: argv[0] is whatever the caller chose to put there, and
// a scan must not be steerable by it.
std::string ownExecutablePath()
{
#if defined(_WIN32)
    // GetModuleFileNameW with a null module is the executable. It truncates rather than failing
    // when the buffer is too small and says so only through ERROR_INSUFFICIENT_BUFFER, so grow
    // until it fits instead of trusting MAX_PATH.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            buf.resize(n);
            try {
                return std::filesystem::path(buf).string();
            } catch (...) {
                return {};
            }
        }
        if (buf.size() >= 32768) // the Win32 long-path ceiling
            return {};
        buf.resize(buf.size() * 2);
    }
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
    return p.string();
#endif
}

//------------------------------------------------------------------------
// Is this path something the scan can actually spawn? On POSIX that is the execute bit. Windows has
// no such bit — an executable is one because of what is inside it — so there the question is only
// whether the file is there at all, and CreateProcess is what finally answers it.
bool isRunnableFile(const std::string &path)
{
#if defined(_WIN32)
    std::wstring wide;
    if (!utf8ToWide(path, wide) || wide.empty())
        return false;
    const DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

//------------------------------------------------------------------------
void collectBundles(const std::string &root, int depth, std::vector<std::string> &out)
{
    if (depth <= 0 || static_cast<int>(out.size()) >= kMaxCandidates)
        return;

    std::error_code ec;
    std::filesystem::directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return;

    for (const auto &entry : it) {
        if (static_cast<int>(out.size()) >= kMaxCandidates)
            return;

        std::error_code e;
        const auto st = entry.symlink_status(e);
        if (e || std::filesystem::is_symlink(st))
            continue; // never follow a symlink during discovery

        const std::string path = entry.path().string();
        if (entry.path().extension() == ".vst3") {
            if (pathIsSafe(path))
                out.push_back(path);
            continue; // a bundle is not searched for nested bundles
        }
        if (std::filesystem::is_directory(st))
            collectBundles(path, depth - 1, out);
    }
}

} // namespace

//------------------------------------------------------------------------
std::vector<std::string> vst3BundlePaths(const std::vector<std::string> &extraRoots)
{
    // Already bundles, not roots — see the header. Anything unsafe is dropped here rather than
    // deeper in, so nothing downstream has to re-check.
    std::vector<std::string> bundles;
    for (auto &p : VST3::Hosting::Module::getModulePaths()) {
        if (pathIsSafe(p))
            bundles.push_back(std::move(p));
    }

    // $VST3_PATH, by contrast, IS a separator-delimited list of roots, so those get walked. The
    // separator is the platform's own — ';' on Windows, where ':' is the second character of every
    // absolute path there is, and splitting on it would turn "C:\\VST3" into "C" and "\\VST3".
    if (const char *extra = std::getenv("VST3_PATH"); extra && extra[0]) {
#if defined(_WIN32)
        constexpr char kPathListSeparator = ';';
#else
        constexpr char kPathListSeparator = ':';
#endif
        const std::string all(extra);
        size_t start = 0;
        while (start <= all.size()) {
            const size_t sep = all.find(kPathListSeparator, start);
            const std::string root =
                all.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            if (!root.empty())
                collectBundles(root, kMaxWalkDepth, bundles);
            if (sep == std::string::npos)
                break;
            start = sep + 1;
        }
    }

    // ...and so are the user's own folders, walked identically. A folder holding no .vst3 at all is
    // not an error: the same root is offered to the LV2 loader, and most will be one or the other.
    for (const std::string &root : extraRoots) {
        if (pathIsSafe(root))
            collectBundles(root, kMaxWalkDepth, bundles);
    }

    // The two sources can name the same bundle; probing it twice would produce duplicate catalogue
    // rows and duplicate cache lines.
    std::sort(bundles.begin(), bundles.end());
    bundles.erase(std::unique(bundles.begin(), bundles.end()), bundles.end());
    return bundles;
}

//------------------------------------------------------------------------
std::vector<std::string> automaticVst3Roots()
{
    // The parent of every bundle the SDK's own enumeration found, which is the set of directories a
    // user does NOT need to add by hand. Reported rather than assumed — see the header.
    std::vector<std::string> roots;
    for (const auto &bundle : VST3::Hosting::Module::getModulePaths()) {
        if (!pathIsSafe(bundle))
            continue;
        std::string parent = std::filesystem::path(bundle).parent_path().string();
        if (!parent.empty())
            roots.push_back(std::move(parent));
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

//------------------------------------------------------------------------
std::vector<std::string> automaticLv2Roots()
{
#if NAMPRACK_HAVE_LV2
    return lv2BundleRoots();
#else
    // Declared unconditionally so the settings page can build on every platform; empty where
    // there is no lilv to ask. A caller that shows the list gets no rows rather than a #ifdef.
    return {};
#endif
}

//------------------------------------------------------------------------
std::vector<std::string> describeVst3Bundle(const std::string &bundlePath)
{
    std::vector<std::string> lines;
    if (!pathIsSafe(bundlePath))
        return lines;

    std::string error;
    auto module = VST3::Hosting::Module::create(bundlePath, error);
    if (!module)
        return lines;

    for (const auto &info : module->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass)
            continue;

        // The payload deliberately omits the path: it is already the cache row's key, and repeating
        // it would let a moved bundle keep a stale copy inside its own record.
        std::string line = formatTag(PluginFormat::Vst3);
        line += '\t';
        line += info.ID().toString(false);
        line += '\t';
        line += info.name();
        line += '\t';
        line += info.subCategoriesString();
        lines.push_back(line);
    }
    return lines;
}

//------------------------------------------------------------------------
bool parsePayload(const std::string &path, const std::string &payload, PluginDesc &out)
{
    // <FORMAT>\t<key-suffix>\t<name>\t<category>
    const size_t t1 = payload.find('\t');
    if (t1 == std::string::npos)
        return false;
    const size_t t2 = payload.find('\t', t1 + 1);
    if (t2 == std::string::npos)
        return false;
    const size_t t3 = payload.find('\t', t2 + 1);
    if (t3 == std::string::npos)
        return false;

    PluginFormat format = PluginFormat::Vst3;
    if (!formatFromTag(payload.substr(0, t1).c_str(), format))
        return false;

    const std::string suffix = payload.substr(t1 + 1, t2 - t1 - 1);
    if (suffix.empty())
        return false;

    out.ref.format = format;
    switch (format) {
        case PluginFormat::Vst3:
            out.ref.key = makeVst3Key(path, suffix);
            break;
        case PluginFormat::Lv2:
            // An LV2 key is the plug-in URI and carries no path.
            out.ref.key = suffix;
            break;
        case PluginFormat::Vst2:
            out.ref.key = path;
            break;
        case PluginFormat::Count:
            return false;
    }

    out.name = payload.substr(t2 + 1, t3 - t2 - 1);
    out.category = payload.substr(t3 + 1);
    return !out.name.empty();
}

//------------------------------------------------------------------------
std::vector<std::string> Catalog::findScanHelper()
{
    // An explicit helper wins, so the one case that cannot re-exec itself can be pointed at a
    // binary that can. It is spawned with the same flag and the same protocol.
    if (const char *env = std::getenv("NAMPRACK_SCAN_HELPER"); env && env[0]) {
        if (isRunnableFile(env))
            return {env, kScanChildFlag};
    }

    // Otherwise this binary, if main() installed the mode. Without that check a re-exec would
    // start a second copy of whatever this process is, which is precisely the accident the flag
    // must not be able to cause.
    if (selfScanAvailable()) {
        const std::string self = ownExecutablePath();
        if (!self.empty() && isRunnableFile(self))
            return {self, kScanChildFlag};
    }
    return {};
}

//------------------------------------------------------------------------
bool Catalog::scanViaHelper(const std::vector<std::string> &helper, const std::string &bundlePath,
                            std::vector<std::string> &lines) const
{
    lines.clear();
    if (helper.empty())
        return false;

    // <command> [flag] <FORMAT> <path>, as scanchild.h documents. The vector is built first so
    // every element outlives the spawn.
    std::vector<std::string> args = helper;
    args.push_back(formatTag(PluginFormat::Vst3));
    args.push_back(bundlePath);

    // What the child printed, and whether it broke the deadline or the output cap. Filled by one of
    // the two blocks below; everything after them is common.
    std::string output;
    bool overran = false;

#if defined(_WIN32)
    // The Win32 half. Same protocol, same deadline, same output cap, same "record the failure and
    // never retry" outcome — only the spawn is different, because Windows has no fork and its
    // anonymous pipes cannot be waited on with the process.
    std::wstring commandLine;
    std::wstring application;
    if (!utf8ToWide(args[0], application))
        return false;
    for (const std::string &arg : args) {
        std::wstring wide;
        if (!utf8ToWide(arg, wide))
            return false;
        appendCommandLineArg(commandLine, wide);
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // the write end has to reach the child

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
        return false;
    // Our end must NOT be inherited. A child holding a copy of the read handle is a pipe that never
    // reports end-of-file, so the loop below would sit on the deadline for every bundle scanned.
    if (!SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    // The child's diagnostics go where ours do, and its stdin is ours. Either may be null in a
    // windowed process with no console attached, which STARTF_USESTDHANDLES treats as "no handle" —
    // the child then has no stderr, which is the same thing it would have had anyway.
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    // CREATE_NO_WINDOW: the child is a console program and the standalone is not, so without it
    // every bundle scanned flashes a console window on the user's desktop.
    //
    // The command line is passed as a writable buffer because CreateProcessW may modify it in place
    // (documented), and a string literal or a shared buffer would be undefined behaviour.
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    const BOOL spawned =
        CreateProcessW(application.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Closed immediately either way: while this process holds a write end of its own, the pipe can
    // never reach end-of-file.
    CloseHandle(writeEnd);
    if (!spawned) {
        CloseHandle(readEnd);
        return false;
    }
    CloseHandle(pi.hThread);

    // Read to end-of-file rather than waiting on the child, for the reason the POSIX side gives: a
    // bridged plug-in can spawn a grandchild that inherits stdout and outlives it, and a wait that
    // returned first would leave the read below blocking on a pipe still held open.
    //
    // PeekNamedPipe is the poll() of this platform — it works on an anonymous pipe, it never
    // consumes, and a closed write end surfaces as ERROR_BROKEN_PIPE, which is what end-of-file is
    // here. Overlapped I/O is not an option: CreatePipe cannot produce an asynchronous handle.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kHelperTimeoutMs);
    bool childExited = false;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            overran = true;
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        // A slice rather than the whole remaining time, so the deadline is still enforced on a
        // child that dribbles output out slowly enough to keep every single wait satisfied.
        const DWORD slice = static_cast<DWORD>(std::min<long long>(remaining, 20));

        DWORD available = 0;
        if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE)
                overran = true; // an unreadable pipe is a scan that produced nothing usable
            break;              // either way there is no more to read
        }

        if (available == 0) {
            if (childExited) {
                // The child is gone but something it started still holds the write end. Nothing to
                // wait on, so sleep the slice instead of spinning on the peek.
                Sleep(slice);
            } else if (WaitForSingleObject(pi.hProcess, slice) == WAIT_OBJECT_0) {
                childExited = true;
            }
            continue;
        }

        char buf[4096];
        DWORD wanted = static_cast<DWORD>(std::min<DWORD>(available, sizeof(buf)));
        DWORD got = 0;
        if (!ReadFile(readEnd, buf, wanted, &got, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE)
                overran = true;
            break;
        }
        if (got == 0)
            break;
        output.append(buf, got);
        if (output.size() > kHelperMaxOutput) {
            overran = true;
            break;
        }
    }
    CloseHandle(readEnd);

    if (overran) {
        // TerminateProcess reaches the child and not its children, which is exactly what
        // kill(pid, SIGKILL) does on the other side; a grandchild that outlives the scan is not
        // holding anything of ours open by then, because the read end is already closed.
        TerminateProcess(pi.hProcess, 1);
        std::fprintf(stderr, "namp-rack: scan of %s timed out or overran; skipping it\n",
                     bundlePath.c_str());
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    const bool haveExitCode = GetExitCodeProcess(pi.hProcess, &exitCode) != FALSE;
    CloseHandle(pi.hProcess);

    if (overran)
        return false;
    if (!haveExitCode || exitCode != 0) {
        std::fprintf(stderr, "namp-rack: scan of %s failed; skipping it\n", bundlePath.c_str());
        return false;
    }
#else
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return false;

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto &arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);

    if (rc != 0) {
        close(fds[0]);
        return false;
    }

    // Read to EOF rather than waiting on the child. A bridged plug-in can spawn a grandchild that
    // inherits stdout and outlives it; waiting on the direct child would then return while the pipe
    // is still open, and reading afterwards would block forever.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kHelperTimeoutMs);

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            overran = true;
            break;
        }
        const int waitMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        struct pollfd pfd = {fds[0], POLLIN, 0};
        const int pr = poll(&pfd, 1, waitMs);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            overran = true;
            break;
        }
        if (pr == 0) {
            overran = true;
            break;
        }

        char buf[4096];
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            overran = true;
            break;
        }
        if (n == 0)
            break; // EOF: the child and anything holding its stdout are done
        output.append(buf, static_cast<size_t>(n));
        if (output.size() > kHelperMaxOutput) {
            overran = true;
            break;
        }
    }
    close(fds[0]);

    if (overran) {
        kill(pid, SIGKILL);
        std::fprintf(stderr, "namp-rack: scan of %s timed out or overran; skipping it\n",
                     bundlePath.c_str());
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (overran)
        return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "namp-rack: scan of %s failed; skipping it\n", bundlePath.c_str());
        return false;
    }
#endif

    size_t start = 0;
    while (start < output.size()) {
        const size_t nl = output.find('\n', start);
        std::string line =
            output.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // A carriage return is stripped rather than kept, and it is not hypothetical: a Windows
        // console child's stdout is a TEXT-mode stream, so every '\n' it printed arrives as
        // "\r\n". Left in place it would ride along on the last tab-separated field — the plug-in's
        // category — and the catalogue row would carry it forever after.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return true;
}

//------------------------------------------------------------------------
void Catalog::rescan(const std::function<void(const ScanProgress &)> &onProgress)
{
    mEntries.clear();
    mProbed = 0;
    mNewCount = 0;

    mCachePath = defaultCachePath();
    mCache.load(mCachePath);

    const std::vector<std::string> extraRoots =
        mPaths ? mPaths->roots() : std::vector<std::string>();
    std::vector<std::string> bundles = vst3BundlePaths(extraRoots);

    const std::vector<std::string> helper = findScanHelper();
    static bool warnedNoHelper = false;
    if (helper.empty() && !warnedNoHelper) {
        // Flagged, not papered over: without a child process a crashing plug-in takes the rack
        // with it.
        std::fprintf(stderr,
                     "namp-rack: no scan helper available; scanning in process. A plug-in that "
                     "crashes during discovery will take this process with it. Call "
                     "runScanChildIfRequested() from main(), or set $NAMPRACK_SCAN_HELPER.\n");
        warnedNoHelper = true;
    }

    int index = 0;
    for (const auto &bundle : bundles) {
        ++index;
        if (onProgress)
            onProgress(ScanProgress{index, static_cast<int>(bundles.size()), bundle});

        const int64_t mtime = newestMTime(bundle);

        std::vector<std::string> lines;
        if (!mCache.hit(bundle, mtime, lines)) {
            ++mProbed;
            const bool ok = helper.empty() ? (lines = describeVst3Bundle(bundle), true)
                                           : scanViaHelper(helper, bundle, lines);
            if (!ok)
                lines.clear();
            // A failure is stored as an empty result on purpose: a broken plug-in should be probed
            // once, not on every launch. It reappears the moment its modification time changes.
            mCache.store(bundle, mtime, lines);
        }

        for (const auto &line : lines) {
            PluginDesc desc;
            if (parsePayload(bundle, line, desc))
                mEntries.push_back(std::move(desc));
        }
    }

    if (!mCachePath.empty() && !mCache.save(mCachePath))
        std::fprintf(stderr, "namp-rack: could not write the plug-in cache to %s\n",
                     mCachePath.c_str());

    // LV2 last, and outside the cache entirely. Discovery there parses Turtle and opens no plug-in
    // binary, so there is no crash to isolate and nothing expensive to remember — see the note at
    // the top of lv2world.h. The path passed to parsePayload is empty because an LV2 key is a URI
    // and carries no path; that is what makes an LV2 node in a saved chain survive its bundle
    // moving.
    // The user's extra roots are offered to lilv first, and are additive: a bundle already on the
    // standard path is not loaded twice, and nothing about the standard path is disturbed. Called
    // even when the list is empty, because that is also how a root the user has just removed stops
    // being listed — see Lv2World::loadRoots.
#if NAMPRACK_HAVE_LV2
    loadLv2Roots(extraRoots);

    for (const std::string &line : describeLv2Plugins()) {
        PluginDesc desc;
        if (parsePayload(std::string(), line, desc))
            mEntries.push_back(std::move(desc));
    }
#endif

    diffAgainstIndex();
}

//------------------------------------------------------------------------
std::string Catalog::defaultIndexPath()
{
#if defined(_WIN32)
    // %LOCALAPPDATA%, beside the scan cache and for the same reason — see defaultCachePath() in
    // scancache.cpp. This file is a set of absolute paths measured on this machine.
    if (const char *local = std::getenv("LOCALAPPDATA"); local && local[0])
        return std::string(local) + "\\NAMp-Rack\\pluginindex";
    return {};
#else
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0])
        return std::string(xdg) + "/NAMp-Rack/pluginindex";
    if (const char *home = std::getenv("HOME"); home && home[0])
        return std::string(home) + "/.cache/NAMp-Rack/pluginindex";
    return {};
#endif
}

//------------------------------------------------------------------------
void Catalog::diffAgainstIndex()
{
    // The baseline is the set of identities the last scan ended with, one per line, as
    // "<FORMAT>\t<escaped key>". It is deliberately NOT the scan cache: the cache is per BUNDLE and
    // knows nothing about LV2, which is scanned in process and never touches it.
    const std::string indexPath = defaultIndexPath();
    if (indexPath.empty())
        return;

    std::set<std::string> previous;
    bool hadBaseline = false;

    std::error_code ec;
    const auto size = std::filesystem::file_size(indexPath, ec);
    if (!ec && size <= kMaxIndexBytes) {
        std::ifstream in(indexPath);
        if (in) {
            hadBaseline = true;
            std::string line;
            while (std::getline(in, line) && previous.size() < kMaxIndexEntries) {
                if (!line.empty() && line.size() <= kMaxIndexLineBytes)
                    previous.insert(line);
            }
        }
    }

    auto identity = [](const PluginDesc &desc) {
        return std::string(formatTag(desc.ref.format)) + '\t' + escapeField(desc.ref.key);
    };

    // On a first run every plug-in is absent from an empty baseline, which is true and useless: the
    // whole collection would be flagged. Seed instead, and start reporting from the next scan.
    if (hadBaseline) {
        for (PluginDesc &desc : mEntries) {
            if (previous.find(identity(desc)) == previous.end()) {
                desc.freshlyFound = true;
                ++mNewCount;
            }
        }
    }

    const std::filesystem::path dir = std::filesystem::path(indexPath).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return;
    }
    std::ofstream out(indexPath, std::ios::trunc);
    if (!out) {
        // Not fatal and not worth more than this: the next scan simply finds no baseline and seeds
        // one again, which costs a round of "new" markers, not correctness.
        std::fprintf(stderr, "namp-rack: could not write the plug-in index to %s\n",
                     indexPath.c_str());
        return;
    }
    for (const PluginDesc &desc : mEntries)
        out << identity(desc) << '\n';
}

} // namespace NAMp::host
