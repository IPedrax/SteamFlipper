#include "AppUpdater.h"

#include "SteamFlipperBuildInfo.h"
#include "SFPlatform/include/DynamicLibrary.h"
#include "SFPlatform/include/Hash.h"
#include "SFPlatform/include/Process.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/Mirror.h"

#include <toml++/toml.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(__linux__)
#include "SFPlatform/include/Http.h"
#include "Utils/Config/Config.h"

#include <dlfcn.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#endif

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace AppUpdater {

namespace {
    // Bounds for a sane framework DLL payload — guards against a truncated or hostile
    // response being written to disk. SteamFlipper is ~1-2 MB.
    constexpr size_t kMinDllBytes = 200 * 1024;
    constexpr size_t kMaxDllBytes = 16 * 1024 * 1024;

    constexpr const char* kPointerPath = "steamflipper/latest.toml";

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

#ifdef _WIN32
    std::string EscapeForPowerShellSingleQuoted(std::string s)
    {
        // Inside a PowerShell single-quoted string, ' is escaped by doubling it.
        for (size_t p = s.find('\''); p != std::string::npos; p = s.find('\'', p + 2))
            s.insert(p, 1, '\'');
        return s;
    }
#endif

#if defined(__linux__)

    // The commit at the head of a branch. Public, unauthenticated, and the
    // reply's first two fields are the sha and the commit message.
    constexpr const char* kCommitsApi =
        "https://api.github.com/repos/IPedrax/SteamFlipper/commits/";

    // dladdr resolves an address against the mappings of loaded objects, so it
    // needs one that lies inside this module. A file-scope object is the safe
    // choice: unlike an exported function, no other loaded library can be
    // holding the symbol that &f would resolve to.
    char g_selfAnchor = 0;

    // Value of a JSON string field, unescaped. Deliberately minimal, in the
    // same spirit as LuaFlipperUI's: the only document parsed here is GitHub's
    // machine-generated commit reply, and only its first "sha" and first
    // "message" are ever read.
    std::string JsonField(const std::string& doc, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t k = doc.find(needle);
        if (k == std::string::npos) return {};
        const size_t c = doc.find(':', k + needle.size());
        if (c == std::string::npos) return {};
        const size_t q = doc.find('"', c);
        if (q == std::string::npos) return {};

        std::string out;
        for (size_t i = q + 1; i < doc.size(); ++i) {
            if (doc[i] == '\\' && i + 1 < doc.size()) {
                const char n = doc[++i];
                out += (n == 'n') ? '\n' : (n == 't') ? '\t' : (n == 'r') ? '\r' : n;
                continue;
            }
            if (doc[i] == '"') break;
            out += doc[i];
        }
        return out;
    }

    // A commit's subject. The body below it is often many paragraphs and this
    // ends up on one row of a settings page.
    std::string FirstLine(std::string s)
    {
        if (const size_t nl = s.find('\n'); nl != std::string::npos) s.resize(nl);
        if (s.size() > 200) s.resize(200);
        return s;
    }

    std::string Lowered(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // A path going into a /bin/sh command line. Single quotes disable every
    // other expansion, so the only character needing work is the quote itself.
    std::string ShellQuote(const std::string& s)
    {
        std::string q = "'";
        for (const char c : s) {
            if (c == '\'') q += "'\\''";
            else           q += c;
        }
        return q + "'";
    }

    /*
     * Run one git command in `repo` and capture its combined output.
     *
     * The two env scrubs are not optional. Steam pins the steam-runtime's
     * older libraries through LD_LIBRARY_PATH and reaches this module through
     * a proxied libXtst, and a child inherits both; the host's git is linked
     * against the host's glibc and dies on the runtime's copy of it.
     *
     * The two prompt kills are what keeps this from hanging: an https remote
     * that wants credentials would otherwise sit forever waiting on a terminal
     * that does not exist, or raise an askpass window over the Steam client.
     *
     * Returns the exit status, or -1 if the command could not be run at all.
     * 127 is /bin/sh's "command not found", i.e. no git on PATH.
     */
    int RunGit(const std::string& repo, const std::string& args, std::string& out)
    {
        const std::string cmd =
            "env -u LD_PRELOAD -u LD_LIBRARY_PATH "
            "GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/true SSH_ASKPASS=/bin/true "
            "git -C " + ShellQuote(repo) + " " + args + " 2>&1";

        out.clear();
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return -1;

        char buf[512];
        while (std::fgets(buf, sizeof(buf), p)) {
            // Bounded: a pull of a large range prints a progress line per
            // object, and none of it past the first few KiB reaches the user.
            if (out.size() < 8192) out += buf;
        }
        const int rc = pclose(p);
        return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    }

    std::string GitHead(const std::string& repo)
    {
        std::string out;
        if (RunGit(repo, "rev-parse --short HEAD", out) != 0) return {};
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

#endif // __linux__
} // namespace

CheckResult Check()
{
    CheckResult r;
    r.oldVersion = STEAMFLIPPER_VERSION;

    std::optional<std::string> body = Mirror::Fetch(kPointerPath);
    if (!body) {
        LOG_WARN("AppUpdater: {} unavailable from all mirrors", kPointerPath);
        return r;
    }

    toml::table tbl;
    try {
        tbl = toml::parse(*body);
    } catch (const toml::parse_error& e) {
        LOG_WARN("AppUpdater: latest.toml parse error: {}", e.description());
        return r;
    }

    const auto version = tbl["version"].value<std::string>();
    const auto path    = tbl["path"].value<std::string>();
    const auto sha     = tbl["sha256"].value<std::string>();
    if (!version || !path || !sha || version->empty() || path->empty() || sha->empty()) {
        LOG_WARN("AppUpdater: latest.toml missing version/path/sha256");
        return r;
    }

    r.newVersion = *version;
    r.dllRelPath = *path;
    r.sha256     = *sha;

    // latest.toml is authoritative for "newest"; build tags are not semver, so any
    // difference from the running version means there is something new to stage.
    if (r.newVersion == r.oldVersion) {
        LOG_INFO("AppUpdater: up to date (version {})", r.oldVersion);
        return r;
    }

    LOG_INFO("AppUpdater: update available {} -> {}", r.oldVersion, r.newVersion);
    r.updateAvailable = true;
    return r;
}

bool DownloadAndStage(const CheckResult& result, const std::string& selfDllPath)
{
    std::optional<std::string> download = Mirror::Fetch(result.dllRelPath);
    if (!download) {
        LOG_WARN("AppUpdater: library download failed for {}", result.dllRelPath);
        return false;
    }
    const std::string& body = *download;

    if (body.size() < kMinDllBytes || body.size() > kMaxDllBytes) {
        LOG_WARN("AppUpdater: rejected library (suspicious size {} bytes)", body.size());
        return false;
    }

    const std::string actual = SFPlatform::Hash::Sha256OfBuffer(body.data(), body.size());
    if (actual.empty() || !EqualsIgnoreCase(actual, result.sha256)) {
        LOG_WARN("AppUpdater: SHA-256 mismatch (expected {}, got {})", result.sha256, actual);
        return false;
    }

    const std::string backup = selfDllPath + ".old";
    std::error_code ec;
    std::filesystem::rename(selfDllPath, backup, ec);
    if (ec) {
        LOG_WARN("AppUpdater: could not rename current library to backup: {}", ec.message());
        return false;
    }

    {
        std::ofstream ofs(selfDllPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            LOG_WARN("AppUpdater: could not open {} for writing; restoring backup", selfDllPath);
            std::filesystem::rename(backup, selfDllPath, ec);
            return false;
        }
        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        ofs.flush();
        if (!ofs) {
            LOG_WARN("AppUpdater: write failed for {}; restoring backup", selfDllPath);
            ofs.close();
            std::filesystem::rename(backup, selfDllPath, ec);
            return false;
        }
    }

    LOG_INFO("AppUpdater: staged {} ({} bytes); applies on next Steam start",
             selfDllPath, body.size());
    return true;
}

void CleanupStagedBackup(const std::string& selfDllPath)
{
    const std::string backup = selfDllPath + ".old";
    std::error_code ec;
    if (std::filesystem::exists(backup, ec)) {
        std::filesystem::remove(backup, ec);
        if (!ec) {
            LOG_INFO("AppUpdater: removed stale backup {}", backup);
        }
    }
}

void RestartSteam()
{
    const std::filesystem::path steamExe = SFPlatform::DynamicLibrary::GetMainExecutablePath();
    if (steamExe.empty()) {
        LOG_WARN("AppUpdater: steam binary path unknown; cannot auto-restart");
        return;
    }

#ifdef _WIN32
    const std::string exe = EscapeForPowerShellSingleQuoted(steamExe.string());
    const std::string command =
        "powershell -NoProfile -WindowStyle Hidden -Command "
        "\"& '" + exe + "' -shutdown; "
        "Wait-Process -Name steam -Timeout 30 -ErrorAction SilentlyContinue; "
        "Start-Process '" + exe + "'\"";
#else
    const std::string command = "steam -shutdown && sleep 2 && steam &";
#endif

    if (SFPlatform::Process::LaunchDetachedHidden(command))
        LOG_INFO("AppUpdater: restart helper launched");
    else
        LOG_WARN("AppUpdater: restart helper failed to launch; "
                 "update applies on next manual Steam start");
}

#if defined(__linux__)

std::string SelfPath()
{
    Dl_info info{};
    char resolved[PATH_MAX];
    if (dladdr(&g_selfAnchor, &info) && info.dli_fname && info.dli_fname[0]) {
        // dli_fname is the name the loader was given, which can be relative;
        // realpath makes it something a log line can be acted on.
        const char* p = realpath(info.dli_fname, resolved) ? resolved : info.dli_fname;
        LOG_INFO("AppUpdater: self path from dladdr: {}", p);
        return p;
    }

    // Both fallbacks are the bootstrap's own candidates, in its order, so a
    // guess still lands on the file that was actually loaded.
    if (const char* env = std::getenv("SF_RUNTIME_PATH"); env && env[0]) {
        LOG_WARN("AppUpdater: dladdr failed, self path from $SF_RUNTIME_PATH: {}", env);
        return env;
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        const std::string p =
            std::string(home) + "/.local/lib/steamflipper/32/SteamFlipper.so";
        LOG_WARN("AppUpdater: dladdr failed, self path from the installer default: {}", p);
        return p;
    }

    LOG_ERROR("AppUpdater: could not resolve the running module's own path");
    return {};
}

SourceCheck CheckSource()
{
    SourceCheck c;
    c.sha    = STEAMFLIPPER_GIT_SHA;
    c.branch = STEAMFLIPPER_GIT_BRANCH;

    if (c.sha.empty() || c.sha == "unknown" || c.branch == "unknown") {
        c.sha = "unknown";
        c.reason = "built outside a git tree, so there is no commit to compare";
        LOG_INFO("AppUpdater: {}", c.reason);
        return c;
    }

    const std::string url = std::string(kCommitsApi) + c.branch;
    // The commit document carries the whole changed-file list, so it dwarfs the
    // three fields read out of it. The cap is generous rather than tight
    // because a truncated body still parses -- sha and message are the first
    // two fields -- while a refused one gives nothing at all.
    const auto r = SFPlatform::Http::Execute(
        L"GET", url.c_str(), nullptr, 0, L"Accept: application/vnd.github+json",
        5000, 5000, 10000, 10000, 2u * 1024 * 1024);

    if (!r.ok) {
        c.error = "GitHub is unreachable";
        LOG_WARN("AppUpdater: {} ({})", c.error, url);
        return c;
    }
    if (r.status == 404) {
        // Not an error. A branch nobody pushed is simply nothing to update to.
        c.reason = "no branch '" + c.branch + "' published on GitHub";
        LOG_INFO("AppUpdater: {}", c.reason);
        return c;
    }
    if (r.status == 403 || r.status == 429) {
        // The API allows 60 unauthenticated calls an hour per address, and the
        // startup check spends one of them per Steam start.
        c.error = "GitHub rate limit reached, try again later";
        LOG_WARN("AppUpdater: {}", c.error);
        return c;
    }
    if (r.status != 200) {
        c.error = "GitHub answered HTTP " + std::to_string(r.status);
        LOG_WARN("AppUpdater: {}", c.error);
        return c;
    }

    const std::string head = JsonField(r.body, "sha");
    if (head.size() < 7) {
        c.error = "GitHub's reply carried no commit sha";
        LOG_WARN("AppUpdater: {}", c.error);
        return c;
    }

    // Shown at the width the local sha was abbreviated to, so two equal shas
    // cannot look different on the page.
    c.remote  = head.substr(0, c.sha.size() < 7 ? 7 : c.sha.size());
    c.message = FirstLine(JsonField(r.body, "message"));
    // "differs from what this was built from" rather than a strict ancestry
    // test: deciding direction needs the remote objects locally, which is a
    // fetch, and this call is deliberately read-only.
    c.behind  = head.compare(0, c.sha.size(), c.sha) != 0;

    LOG_INFO("AppUpdater: {}@{} vs origin {} -> {}", c.branch, c.sha, c.remote,
             c.behind ? "update available" : "up to date");
    return c;
}

PullResult PullSource()
{
    PullResult p;
    p.repo = Config::GetUpdateRepo();

    if (p.repo.empty()) {
        p.status = "no-repo";
        p.error  = "the source tree is unknown: set repo = \"/path/to/SteamFlipper\" "
                   "under [update] in steamflipper.toml";
        LOG_WARN("AppUpdater: {}", p.error);
        return p;
    }

    std::string out;
    const int probe = RunGit(p.repo, "rev-parse --is-inside-work-tree", out);
    if (probe == 127 || probe < 0) {
        p.status = "no-git";
        p.error  = "git is not installed or could not be run";
        LOG_WARN("AppUpdater: {}", p.error);
        return p;
    }
    if (probe != 0) {
        p.status = "no-repo";
        p.error  = p.repo + " is not a git checkout";
        LOG_WARN("AppUpdater: {}", p.error);
        return p;
    }

    // Tracked changes only. Untracked files do not block a fast-forward unless
    // an incoming file would land on one, and git refuses that case itself
    // with a message worth more than a blanket "the tree is dirty".
    if (RunGit(p.repo, "status --porcelain --untracked-files=no", out) == 0 &&
        !out.empty()) {
        p.status = "dirty-tree";
        p.error  = "the working tree has local changes; commit or stash them first";
        p.sha    = GitHead(p.repo);
        LOG_WARN("AppUpdater: refusing to pull, {} has local changes", p.repo);
        return p;
    }

    const std::string before = GitHead(p.repo);

    // --ff-only is the whole safety story: it can fast-forward or refuse, and
    // there is no third outcome that rewrites or merges anything.
    //
    // The two http.lowSpeed settings are the only thing bounding this call.
    // git has no transfer timeout of its own, the API server answers requests
    // one at a time, and a stalled fetch would therefore take the whole UI
    // down with it rather than just this button.
    const int rc = RunGit(
        p.repo, "-c http.lowSpeedLimit=1000 -c http.lowSpeedTime=20 pull --ff-only", out);
    p.sha = GitHead(p.repo);

    if (rc != 0) {
        const std::string low = Lowered(out);
        if (low.find("fast-forward") != std::string::npos ||
            low.find("diverge") != std::string::npos) {
            p.status = "not-fast-forward";
            p.error  = "the branch has moved in a way a fast-forward cannot follow; "
                       "reconcile it by hand";
        } else if (low.find("could not resolve host") != std::string::npos ||
                   low.find("unable to access") != std::string::npos ||
                   low.find("could not read from remote") != std::string::npos ||
                   low.find("connection") != std::string::npos ||
                   low.find("network") != std::string::npos) {
            p.status = "network-failed";
            p.error  = "could not reach the remote";
        } else {
            p.status = "failed";
            p.error  = FirstLine(out);
            if (p.error.empty()) p.error = "git pull failed";
        }
        LOG_WARN("AppUpdater: git pull --ff-only in {} failed ({}): {}",
                 p.repo, p.status, FirstLine(out));
        return p;
    }

    p.ok     = true;
    p.status = (p.sha == before) ? "already-current" : "pulled";
    LOG_INFO("AppUpdater: {} is now at {} ({})", p.repo, p.sha, p.status);
    return p;
}

#endif // __linux__

} // namespace AppUpdater
