#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct Config {
    fs::path root;
    fs::path backupRoot = "/backup";
    std::string request;
    std::string model = "gpt-4.1";
    size_t maxProjectBytes = 220000;
    size_t maxFileBytes = 60000;
    bool allowDelete = false;
    bool allowRename = false;
    bool applyLast = false;
    bool rollback = false;
    bool noSyntaxCheck = false;
};

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read file: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write file: " + p.string());
    out << s;
}

static std::string nowStamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);

    int hour = tm.tm_hour;
    const char* ampm = hour >= 12 ? "pm" : "am";
    hour %= 12;
    if (hour == 0) hour = 12;

    char buf[64];
    std::snprintf(
        buf,
        sizeof(buf),
        "%d-%d-%d-%d:%02d%s",
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_year + 1900,
        hour,
        tm.tm_min,
        ampm
    );

    return buf;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static fs::path canonicalSafe(const fs::path& p) {
    return fs::weakly_canonical(p);
}

static bool isInsideRoot(const fs::path& root, const fs::path& target) {
    auto r = canonicalSafe(root).string();
    auto t = canonicalSafe(target).string();

    if (!r.empty() && r.back() != '/') r += "/";

    return t == canonicalSafe(root).string() || startsWith(t, r);
}

static bool shouldSkipDir(const fs::path& p) {
    static std::set<std::string> skip = {
        ".git", ".ai-edit", "node_modules", "vendor", "cache", "tmp", "logs",
        "uploads", "backup", "backups", ".idea", ".vscode"
    };

    return skip.count(p.filename().string()) > 0;
}

static bool allowedExt(const fs::path& p) {
    static std::set<std::string> exts = {
        ".php", ".inc", ".html", ".htm", ".css", ".js", ".json", ".sql",
        ".txt", ".md", ".xml", ".yml", ".yaml", ".conf", ".ini",
        ".cpp", ".h", ".hpp", ".py", ".sh"
    };

    std::string name = p.filename().string();

    if (name == ".htaccess") return true;
    if (name == ".env") return false;

    return exts.count(p.extension().string()) > 0;
}

static int runProcess(const std::vector<std::string>& args, const fs::path& cwd = "") {
    if (args.empty()) return 1;

    pid_t pid = fork();

    if (pid == 0) {
        if (!cwd.empty()) chdir(cwd.c_str());

        std::vector<char*> argv;
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }

        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

static void printUsage() {
    std::cout <<
R"(Usage:
  ai-edit --root /path/project --request "change request"

Rename:
  ai-edit --root /path/project --request "rename old.php to new.php" --allow-rename

Delete:
  ai-edit --root /path/project --request "delete unused.php" --allow-delete

Rollback:
  ai-edit --root /path/project --rollback

Options:
  --model MODEL
  --backup-root /backup
  --max-project-bytes N
  --max-file-bytes N
  --allow-delete
  --allow-rename
  --no-syntax-check
)";
}

static Config parseArgs(int argc, char** argv) {
    Config c;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];

        if (a == "--root" && i + 1 < argc) c.root = argv[++i];
        else if (a == "--request" && i + 1 < argc) c.request = argv[++i];
        else if (a == "--model" && i + 1 < argc) c.model = argv[++i];
        else if (a == "--backup-root" && i + 1 < argc) c.backupRoot = argv[++i];
        else if (a == "--max-project-bytes" && i + 1 < argc) c.maxProjectBytes = std::stoul(argv[++i]);
        else if (a == "--max-file-bytes" && i + 1 < argc) c.maxFileBytes = std::stoul(argv[++i]);
        else if (a == "--allow-delete") c.allowDelete = true;
        else if (a == "--allow-rename") c.allowRename = true;
        else if (a == "--apply-last") c.applyLast = true;
        else if (a == "--rollback") c.rollback = true;
        else if (a == "--no-syntax-check") c.noSyntaxCheck = true;
        else {
            printUsage();
            exit(1);
        }
    }

    if (c.root.empty()) {
        printUsage();
        exit(1);
    }

    c.root = fs::canonical(c.root);

    if (!fs::exists(c.root) || !fs::is_directory(c.root)) {
        throw std::runtime_error("Invalid root directory.");
    }

    if (!c.applyLast && !c.rollback && c.request.empty()) {
        throw std::runtime_error("Missing --request.");
    }

    return c;
}

static fs::path aiDir(const Config& c) {
    return c.root / ".ai-edit";
}

static std::vector<fs::path> listProjectFiles(const Config& c) {
    std::vector<fs::path> files;

    for (auto it = fs::recursive_directory_iterator(c.root); it != fs::recursive_directory_iterator(); ++it) {
        const auto& p = it->path();

        if (it->is_directory()) {
            if (shouldSkipDir(p)) it.disable_recursion_pending();
            continue;
        }

        if (!it->is_regular_file()) continue;
        if (!allowedExt(p)) continue;
        if (fs::file_size(p) > c.maxFileBytes) continue;

        files.push_back(fs::relative(p, c.root));
    }

    std::sort(files.begin(), files.end());
    return files;
}

static std::string buildProjectContext(const Config& c, const std::vector<fs::path>& files) {
    std::ostringstream ctx;
    size_t used = 0;

    ctx << "PROJECT_ROOT: " << c.root.string() << "\n\n";
    ctx << "FILE_TREE:\n";

    for (const auto& f : files) {
        ctx << "- " << f.string() << "\n";
    }

    ctx << "\nFILE_CONTENTS:\n";

    for (const auto& rel : files) {
        fs::path full = c.root / rel;
        std::string content = readFile(full);

        if (used + content.size() > c.maxProjectBytes) {
            ctx << "\n[PROJECT CONTEXT TRUNCATED DUE TO SIZE LIMIT]\n";
            break;
        }

        used += content.size();

        ctx << "\n--- FILE: " << rel.string() << " ---\n";
        ctx << content << "\n";
        ctx << "--- END FILE: " << rel.string() << " ---\n";
    }

    return ctx.str();
}

static size_t curlWrite(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

static std::string extractPatchFromResponse(const json& j) {
    if (j.contains("output_text") && j["output_text"].is_string()) {
        return j["output_text"].get<std::string>();
    }

    std::ostringstream out;

    if (j.contains("output") && j["output"].is_array()) {
        for (const auto& item : j["output"]) {
            if (!item.contains("content")) continue;

            for (const auto& c : item["content"]) {
                if (c.contains("text") && c["text"].is_string()) {
                    out << c["text"].get<std::string>();
                }
            }
        }
    }

    return out.str();
}

static std::string cleanPatch(std::string s) {
    std::regex fence(R"(```(?:diff|patch)?\s*([\s\S]*?)```)");
    std::smatch m;

    if (std::regex_search(s, m, fence)) {
        s = m[1].str();
    }

    auto pos = s.find("diff --git ");
    if (pos != std::string::npos) {
        s = s.substr(pos);
    }

    return s;
}

static std::string callOpenAI(const Config& c, const std::string& projectContext) {
    const char* key = std::getenv("OPENAI_API_KEY");

    if (!key) {
        throw std::runtime_error("Missing OPENAI_API_KEY environment variable.");
    }

    std::string instructions =
        "You are a careful server-side code editor. "
        "Return only a unified git diff. "
        "Do not include explanations. "
        "Do not include shell commands. "
        "Do not modify files outside PROJECT_ROOT. "
        "Do not delete files unless the user explicitly requested deletion. "
        "Do not rename files unless the user explicitly requested renaming. "
        "Make the smallest safe working change. "
        "Preserve existing style. "
        "Avoid secrets. "
        "Do not alter .env files.";

    std::ostringstream user;
    user << "USER_REQUEST:\n" << c.request << "\n\n";
    user << "Return a git-compatible unified diff only.\n\n";
    user << projectContext;

    json body = {
        {"model", c.model},
        {"instructions", instructions},
        {"input", user.str()}
    };

    std::string response;

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed.");

    struct curl_slist* headers = nullptr;
    std::string auth = std::string("Authorization: Bearer ") + key;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());

    std::string payload = body.dump();

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/responses");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("OpenAI request failed: ") + curl_easy_strerror(res));
    }

    if (code < 200 || code >= 300) {
        throw std::runtime_error("OpenAI HTTP error " + std::to_string(code) + ":\n" + response);
    }

    json j = json::parse(response);
    return cleanPatch(extractPatchFromResponse(j));
}

static bool patchHasDelete(const std::string& patch) {
    return patch.find("deleted file mode") != std::string::npos ||
           patch.find("+++ /dev/null") != std::string::npos;
}

static bool patchHasRename(const std::string& patch) {
    return patch.find("rename from ") != std::string::npos ||
           patch.find("rename to ") != std::string::npos;
}

static std::set<fs::path> touchedFiles(const std::string& patch) {
    std::set<fs::path> files;
    std::istringstream in(patch);
    std::string line;

    while (std::getline(in, line)) {
        if (startsWith(line, "+++ b/")) files.insert(line.substr(6));
        if (startsWith(line, "--- a/")) files.insert(line.substr(6));
        if (startsWith(line, "rename from ")) files.insert(line.substr(12));
        if (startsWith(line, "rename to ")) files.insert(line.substr(10));
    }

    files.erase("/dev/null");
    return files;
}

static void validatePatchPaths(const Config& c, const std::string& patch) {
    auto files = touchedFiles(patch);

    for (const auto& rel : files) {
        std::string s = rel.string();

        if (s.empty()) continue;

        if (s.find("..") != std::string::npos) {
            throw std::runtime_error("Patch contains unsafe path: " + s);
        }

        if (s[0] == '/') {
            throw std::runtime_error("Patch contains absolute path: " + s);
        }

        if (s == ".env" || s.find("/.env") != std::string::npos) {
            throw std::runtime_error("Patch attempts to touch .env.");
        }

        fs::path full = c.root / rel;

        if (!isInsideRoot(c.root, full)) {
            throw std::runtime_error("Patch attempts to leave project root: " + s);
        }
    }
}

static void requireConfirm(const std::string& word) {
    std::cout << "Type " << word << " to confirm: ";
    std::string s;
    std::getline(std::cin, s);

    if (s != word) {
        throw std::runtime_error("Confirmation failed.");
    }
}

static void autoBackupTouchedFiles(const Config& c, const std::string& patch, const std::string& session) {
    fs::path publicBackupSessionDir = c.backupRoot / session;
    fs::path internalSessionDir = aiDir(c) / "backups" / session;

    fs::create_directories(publicBackupSessionDir);
    fs::create_directories(internalSessionDir);

    std::ostringstream manifest;
    auto files = touchedFiles(patch);

    for (const auto& rel : files) {
        fs::path source = c.root / rel;

        if (fs::exists(source) && fs::is_regular_file(source)) {
            fs::path publicBackupPath = publicBackupSessionDir / rel;
            fs::path internalBackupPath = internalSessionDir / rel;

            fs::create_directories(publicBackupPath.parent_path());
            fs::create_directories(internalBackupPath.parent_path());

            fs::copy_file(source, publicBackupPath, fs::copy_options::overwrite_existing);
            fs::copy_file(source, internalBackupPath, fs::copy_options::overwrite_existing);

            manifest << "BACKUP " << rel.string() << "\n";
            std::cout << "Auto backup created: " << publicBackupPath.string() << "\n";
        } else {
            manifest << "CREATED_OR_MISSING " << rel.string() << "\n";
        }
    }

    writeFile(internalSessionDir / "manifest.txt", manifest.str());
    writeFile(aiDir(c) / "last-session.txt", session);
}

static void rollbackLast(const Config& c) {
    fs::path sessionFile = aiDir(c) / "last-session.txt";

    if (!fs::exists(sessionFile)) {
        throw std::runtime_error("No rollback session found.");
    }

    std::string session = readFile(sessionFile);
    session.erase(std::remove(session.begin(), session.end(), '\n'), session.end());

    fs::path bdir = aiDir(c) / "backups" / session;
    fs::path manifest = bdir / "manifest.txt";

    if (!fs::exists(manifest)) {
        throw std::runtime_error("Rollback manifest missing.");
    }

    std::istringstream in(readFile(manifest));
    std::string type, rel;

    while (in >> type >> rel) {
        fs::path target = c.root / rel;
        fs::path backup = bdir / rel;

        if (type == "BACKUP" && fs::exists(backup)) {
            fs::create_directories(target.parent_path());
            fs::copy_file(backup, target, fs::copy_options::overwrite_existing);
            std::cout << "Restored " << rel << "\n";
        }
    }

    std::cout << "Rollback complete.\n";
}

static void runSyntaxChecks(const Config& c, const std::string& patch) {
    if (c.noSyntaxCheck) return;

    auto files = touchedFiles(patch);

    for (const auto& rel : files) {
        fs::path full = c.root / rel;

        if (!fs::exists(full)) continue;

        if (full.extension() == ".php") {
            std::cout << "PHP syntax check: " << rel.string() << "\n";

            int rc = runProcess({"php", "-l", full.string()}, c.root);

            if (rc != 0) {
                throw std::runtime_error("PHP syntax check failed for " + rel.string());
            }
        }
    }
}

static void saveLog(const Config& c, const std::string& session, const std::string& patch) {
    fs::create_directories(aiDir(c));

    std::ofstream log(aiDir(c) / "ai-edit.log", std::ios::app);

    log << "\nSESSION " << session << "\n";
    log << "BACKUP_ROOT: " << c.backupRoot.string() << "\n";
    log << "REQUEST: " << c.request << "\n";
    log << "PATCH:\n" << patch << "\n";
}

static void applyPatch(const Config& c, const std::string& patch) {
    validatePatchPaths(c, patch);

    if (patch.empty() || patch.find("diff --git ") == std::string::npos) {
        throw std::runtime_error("No valid git diff returned.");
    }

    if (patchHasDelete(patch) && !c.allowDelete) {
        throw std::runtime_error("Patch includes deletion. Re-run with --allow-delete.");
    }

    if (patchHasRename(patch) && !c.allowRename) {
        throw std::runtime_error("Patch includes rename. Re-run with --allow-rename.");
    }

    std::cout << "\nGenerated patch:\n\n";
    std::cout << patch << "\n";

    if (patchHasDelete(patch)) requireConfirm("DELETE");
    if (patchHasRename(patch)) requireConfirm("RENAME");

    requireConfirm("APPLY");

    std::string session = nowStamp();

    fs::create_directories(aiDir(c));
    fs::create_directories(c.backupRoot);

    fs::path patchPath = aiDir(c) / "last.patch";
    writeFile(patchPath, patch);

    int check = runProcess({"git", "apply", "--check", patchPath.string()}, c.root);

    if (check != 0) {
        throw std::runtime_error("git apply --check failed. Patch was not applied.");
    }

    autoBackupTouchedFiles(c, patch, session);

    int applied = runProcess({"git", "apply", patchPath.string()}, c.root);

    if (applied != 0) {
        throw std::runtime_error("git apply failed after backup. Your original files are backed up.");
    }

    runSyntaxChecks(c, patch);
    saveLog(c, session, patch);

    std::cout << "\nApplied successfully.\n";
    std::cout << "Changed-file backups saved in:\n";
    std::cout << "  " << (c.backupRoot / session).string() << "\n";
    std::cout << "Rollback last change with:\n";
    std::cout << "  ai-edit --root " << c.root.string() << " --rollback\n";
}

int main(int argc, char** argv) {
    try {
        Config c = parseArgs(argc, argv);

        fs::create_directories(aiDir(c));

        if (c.rollback) {
            rollbackLast(c);
            return 0;
        }

        if (c.applyLast) {
            fs::path p = aiDir(c) / "last.patch";

            if (!fs::exists(p)) {
                throw std::runtime_error("No last.patch found.");
            }

            applyPatch(c, readFile(p));
            return 0;
        }

        auto files = listProjectFiles(c);
        auto context = buildProjectContext(c, files);
        auto patch = callOpenAI(c, context);

        writeFile(aiDir(c) / "last.patch", patch);
        applyPatch(c, patch);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nERROR: " << e.what() << "\n";
        return 1;
    }
}
