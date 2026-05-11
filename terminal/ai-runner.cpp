/*
  ai-edit-tui.cpp

  Terminal UI version of AI Edit for SSH and server terminals.

  Features:
  - ncurses terminal interface with file list, preview pane, log pane, and status bar.
  - Uses the OpenAI Responses API with OPENAI_API_KEY from the environment.
  - Scans safe project file types and skips large/generated/sensitive folders.
  - Builds a project context, asks the model for a strict JSON change plan, previews it, then applies it only when approved.
  - Creates backups before applying edits and supports rollback of the last applied edit.
  - Runs syntax checks for PHP, Python, JavaScript, and JSON when possible.

  Build:
    g++ -std=c++20 -O2 -Wall -Wextra ai-edit-tui.cpp -o ai-edit-tui -lcurl -lncurses

  Run:
    export OPENAI_API_KEY="your_api_key_here"
    ./ai-edit-tui /path/to/project
*/

#include <ncurses.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct AppConfig {
    fs::path root;
    fs::path backupRoot = "/backup";
    std::string request;
    std::string model = "gpt-4.1";
    size_t maxProjectBytes = 220000;
    size_t maxFileBytes = 60000;
    bool allowDelete = false;
    bool allowRename = false;
    bool noSyntaxCheck = false;
};

struct FileChange {
    std::string action;
    std::string path;
    std::string newPath;
    std::string content;
};

struct ChangePlan {
    std::string summary;
    std::vector<FileChange> changes;
};

struct UiHooks {
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> changes;
    std::function<void(const std::string&)> prompt;
    std::function<void(const std::string&)> response;
    std::function<void(const std::string&, const std::string&)> fileStatus;
};

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read file: " + p.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Cannot write file: " + p.string());
    }
    out << s;
}

static std::string nowStamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static fs::path canonicalSafe(const fs::path& p) {
    return fs::weakly_canonical(p);
}

static bool isInsideRoot(const fs::path& root, const fs::path& target) {
    std::string r = canonicalSafe(root).string();
    std::string t = canonicalSafe(target).string();
    std::string exactRoot = canonicalSafe(root).string();
    if (!r.empty() && r.back() != '/') {
        r += "/";
    }
    return t == exactRoot || startsWith(t, r);
}

static fs::path aiDir(const AppConfig& c) {
    return c.root / ".ai-edit";
}

static bool shouldSkipDir(const fs::path& p) {
    static const std::set<std::string> skip = {
        ".ai-edit", ".git", "node_modules", "vendor", "cache", "tmp", "logs",
        "uploads", "backup", "backups", ".idea", ".vscode", "dist", "build",
        "target", "__pycache__", ".pytest_cache", ".next", ".nuxt"
    };
    return skip.count(p.filename().string()) > 0;
}

static bool allowedExt(const fs::path& p) {
    static const std::set<std::string> exts = {
        ".php", ".inc", ".html", ".htm", ".css", ".js", ".mjs", ".cjs",
        ".jsx", ".ts", ".tsx", ".json", ".sql", ".txt", ".md", ".xml",
        ".yml", ".yaml", ".conf", ".ini", ".cpp", ".c", ".h", ".hpp",
        ".py", ".sh", ".rb", ".go", ".rs", ".java", ".cs", ".env.example",
        ".dockerfile"
    };
    const std::string name = p.filename().string();
    if (name == ".htaccess") {
        return true;
    }
    if (name == ".env") {
        return false;
    }
    if (name == "Dockerfile") {
        return true;
    }
    return exts.count(p.extension().string()) > 0;
}

static bool pathLooksUnsafe(const std::string& rel) {
    if (rel.empty()) {
        return true;
    }
    if (rel[0] == '/') {
        return true;
    }
    if (rel.find("..") != std::string::npos) {
        return true;
    }
    if (rel == ".env" || rel.find("/.env") != std::string::npos) {
        return true;
    }
    return false;
}

static int runProcessCapture(const std::vector<std::string>& args, const fs::path& cwd, std::string* output) {
    if (args.empty()) {
        return 1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (!cwd.empty()) {
            chdir(cwd.c_str());
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (output) {
            output->append(buf, static_cast<size_t>(n));
        }
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}

static std::vector<fs::path> listProjectFiles(const AppConfig& c, const UiHooks& ui) {
    std::vector<fs::path> files;
    ui.log("Scanning project files...");

    for (auto it = fs::recursive_directory_iterator(c.root); it != fs::recursive_directory_iterator(); ++it) {
        const auto& p = it->path();
        if (it->is_directory()) {
            if (shouldSkipDir(p)) {
                ui.log("Skipping directory: " + fs::relative(p, c.root).string());
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        if (!allowedExt(p)) {
            continue;
        }
        if (fs::file_size(p) > c.maxFileBytes) {
            ui.fileStatus(fs::relative(p, c.root).string(), "skipped, too large");
            continue;
        }
        auto rel = fs::relative(p, c.root);
        files.push_back(rel);
        ui.fileStatus(rel.string(), "scanned");
    }

    std::sort(files.begin(), files.end());
    ui.log("Scan complete, " + std::to_string(files.size()) + " candidate files found.");
    return files;
}

static std::string buildProjectContext(const AppConfig& c, const std::vector<fs::path>& files, const UiHooks& ui) {
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
            ui.log("Project context reached byte limit, remaining files were not sent.");
            break;
        }
        used += content.size();
        ctx << "\n--- FILE: " << rel.string() << " ---\n";
        ctx << content << "\n";
        ctx << "--- END FILE: " << rel.string() << " ---\n";
        ui.fileStatus(rel.string(), "included in OpenAI context");
    }

    ui.log("Project context prepared, " + std::to_string(used) + " bytes of file content included.");
    return ctx.str();
}

static size_t curlWrite(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

static std::string extractTextFromResponse(const json& j) {
    if (j.contains("output_text") && j["output_text"].is_string()) {
        return j["output_text"].get<std::string>();
    }

    std::ostringstream out;
    if (j.contains("output") && j["output"].is_array()) {
        for (const auto& item : j["output"]) {
            if (!item.contains("content")) {
                continue;
            }
            for (const auto& c : item["content"]) {
                if (c.contains("text") && c["text"].is_string()) {
                    out << c["text"].get<std::string>();
                }
            }
        }
    }
    return out.str();
}

static std::string cleanJsonText(std::string s) {
    std::regex fence(R"(```(?:json)?\s*([\s\S]*?)```)");
    std::smatch m;
    if (std::regex_search(s, m, fence)) {
        s = m[1].str();
    }
    auto first = s.find('{');
    auto last = s.rfind('}');
    if (first != std::string::npos && last != std::string::npos && last > first) {
        s = s.substr(first, last - first + 1);
    }
    return s;
}

static ChangePlan parseChangePlan(const std::string& text) {
    std::string cleaned = cleanJsonText(text);
    json j = json::parse(cleaned);

    ChangePlan plan;
    plan.summary = j.value("summary", "");
    if (!j.contains("changes") || !j["changes"].is_array()) {
        throw std::runtime_error("OpenAI response must contain a changes array.");
    }

    for (const auto& item : j["changes"]) {
        FileChange ch;
        ch.action = item.value("action", "");
        ch.path = item.value("path", "");
        ch.newPath = item.value("new_path", "");
        ch.content = item.value("content", "");

        if (ch.action != "write" && ch.action != "create" && ch.action != "delete" && ch.action != "rename") {
            throw std::runtime_error("Unsupported file action in OpenAI response: " + ch.action);
        }
        if (ch.path.empty()) {
            throw std::runtime_error("A file action is missing its path.");
        }
        if (ch.action == "rename" && ch.newPath.empty()) {
            throw std::runtime_error("Rename action is missing new_path.");
        }
        plan.changes.push_back(ch);
    }

    if (plan.changes.empty()) {
        throw std::runtime_error("OpenAI did not return any file changes.");
    }
    return plan;
}

static std::string callOpenAI(const AppConfig& c, const std::string& projectContext, const UiHooks& ui) {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || std::string(key).empty()) {
        throw std::runtime_error("Missing OPENAI_API_KEY environment variable.");
    }

    const std::string instructions =
        "You are a careful Linux project file editor. "
        "Return only strict JSON. Do not include markdown or commentary. "
        "The JSON format must be: "
        "{\"summary\":\"short summary\",\"changes\":["
        "{\"action\":\"write\",\"path\":\"relative/file/path\",\"content\":\"full new file content\"}"
        "]}. "
        "Allowed action values are write, create, delete, and rename. "
        "For rename use {\"action\":\"rename\",\"path\":\"old/path\",\"new_path\":\"new/path\",\"content\":\"optional full content after rename\"}. "
        "For write and create, always provide the complete final file content, not a partial patch. "
        "Never use absolute paths. Never use parent directory traversal. "
        "Do not modify files outside PROJECT_ROOT. Do not touch .env files. "
        "Do not delete files unless the user explicitly requested deletion. "
        "Do not rename files unless the user explicitly requested renaming. "
        "Make safe, working changes. Preserve existing style when possible.";

    std::ostringstream user;
    user << "USER_REQUEST:\n" << c.request << "\n\n";
    user << "Return strict JSON only.\n\n";
    user << projectContext;

    json body = {
        {"model", c.model},
        {"instructions", instructions},
        {"input", user.str()}
    };

    ui.prompt(user.str());
    ui.log("Sending request to OpenAI model: " + c.model);
    ui.response("Waiting for OpenAI response...");

    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl init failed.");
    }

    struct curl_slist* headers = nullptr;
    const std::string auth = std::string("Authorization: Bearer ") + key;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());

    const std::string payload = body.dump();
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/responses");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 240L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ai-edit-tui/1.0");

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("OpenAI request failed: ") + curl_easy_strerror(res));
    }
    if (code < 200 || code >= 300) {
        ui.response(response);
        throw std::runtime_error("OpenAI HTTP error " + std::to_string(code));
    }

    ui.response(response);
    json j = json::parse(response);
    std::string outputText = extractTextFromResponse(j);
    if (outputText.empty()) {
        throw std::runtime_error("OpenAI response did not contain output text.");
    }

    ui.log("OpenAI response received, " + std::to_string(outputText.size()) + " bytes of JSON text.");
    return outputText;
}

static void validateChangePlan(const AppConfig& c, const ChangePlan& plan) {
    for (const auto& ch : plan.changes) {
        if (pathLooksUnsafe(ch.path)) {
            throw std::runtime_error("Unsafe path in change plan: " + ch.path);
        }
        fs::path target = c.root / ch.path;
        if (!isInsideRoot(c.root, target)) {
            throw std::runtime_error("Change attempts to leave project root: " + ch.path);
        }
        if (ch.action == "rename") {
            if (pathLooksUnsafe(ch.newPath)) {
                throw std::runtime_error("Unsafe rename destination in change plan: " + ch.newPath);
            }
            fs::path renamed = c.root / ch.newPath;
            if (!isInsideRoot(c.root, renamed)) {
                throw std::runtime_error("Rename destination leaves project root: " + ch.newPath);
            }
        }
    }
}

static std::set<std::string> touchedFiles(const ChangePlan& plan) {
    std::set<std::string> out;
    for (const auto& ch : plan.changes) {
        out.insert(ch.path);
        if (ch.action == "rename" && !ch.newPath.empty()) {
            out.insert(ch.newPath);
        }
    }
    return out;
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    if (!s.empty() && s.back() == '\n') {
        lines.push_back("");
    }
    return lines;
}

static std::string makeSimpleDiff(const std::string& oldText, const std::string& newText) {
    if (oldText == newText) {
        return " No text changes.\n";
    }

    auto oldLines = splitLines(oldText);
    auto newLines = splitLines(newText);
    std::ostringstream out;
    size_t maxLines = std::max(oldLines.size(), newLines.size());
    size_t changedShown = 0;

    for (size_t i = 0; i < maxLines; ++i) {
        const bool hasOld = i < oldLines.size();
        const bool hasNew = i < newLines.size();
        std::string oldLine = hasOld ? oldLines[i] : "";
        std::string newLine = hasNew ? newLines[i] : "";
        if (hasOld && hasNew && oldLine == newLine) {
            continue;
        }

        out << "@@ line " << (i + 1) << " @@\n";
        if (hasOld) {
            out << "- " << oldLine << "\n";
        }
        if (hasNew) {
            out << "+ " << newLine << "\n";
        }
        ++changedShown;
        if (changedShown >= 500) {
            out << "\n[Change preview truncated after 500 changed lines]\n";
            break;
        }
    }
    return out.str();
}

static std::string renderChangePreview(const AppConfig& c, const ChangePlan& plan) {
    std::ostringstream out;
    out << "SUMMARY:\n" << (plan.summary.empty() ? "No summary provided." : plan.summary) << "\n\n";
    out << "CHANGE PLAN:\n";

    for (const auto& ch : plan.changes) {
        out << "\n============================================================\n";
        out << "ACTION: " << ch.action << "\n";
        out << "PATH: " << ch.path << "\n";
        if (!ch.newPath.empty()) {
            out << "NEW PATH: " << ch.newPath << "\n";
        }
        out << "------------------------------------------------------------\n";

        if (ch.action == "delete") {
            fs::path oldPath = c.root / ch.path;
            if (fs::exists(oldPath) && fs::is_regular_file(oldPath)) {
                out << "File will be deleted. Existing content preview:\n";
                std::string oldText = readFile(oldPath);
                auto lines = splitLines(oldText);
                for (size_t i = 0; i < std::min<size_t>(lines.size(), 200); ++i) {
                    out << "- " << lines[i] << "\n";
                }
                if (lines.size() > 200) {
                    out << "[Deleted file preview truncated]\n";
                }
            } else {
                out << "File does not currently exist.\n";
            }
            continue;
        }

        if (ch.action == "rename") {
            out << "File will be renamed from " << ch.path << " to " << ch.newPath << ".\n";
            if (!ch.content.empty()) {
                fs::path oldPath = c.root / ch.path;
                std::string oldText;
                if (fs::exists(oldPath) && fs::is_regular_file(oldPath)) {
                    oldText = readFile(oldPath);
                }
                out << makeSimpleDiff(oldText, ch.content);
            }
            continue;
        }

        fs::path oldPath = c.root / ch.path;
        std::string oldText;
        if (fs::exists(oldPath) && fs::is_regular_file(oldPath)) {
            oldText = readFile(oldPath);
        }
        out << makeSimpleDiff(oldText, ch.content);
    }
    return out.str();
}

static void saveChangePlan(const AppConfig& c, const ChangePlan& plan) {
    json j;
    j["summary"] = plan.summary;
    j["changes"] = json::array();
    for (const auto& ch : plan.changes) {
        json item;
        item["action"] = ch.action;
        item["path"] = ch.path;
        if (!ch.newPath.empty()) {
            item["new_path"] = ch.newPath;
        }
        if (!ch.content.empty()) {
            item["content"] = ch.content;
        }
        j["changes"].push_back(item);
    }
    fs::create_directories(aiDir(c));
    writeFile(aiDir(c) / "last-change-plan.json", j.dump(2));
}

static void autoBackupTouchedFiles(const AppConfig& c, const ChangePlan& plan, const std::string& session, const UiHooks& ui) {
    fs::path publicBackupSessionDir = c.backupRoot / session;
    fs::path internalSessionDir = aiDir(c) / "backups" / session;
    fs::create_directories(publicBackupSessionDir);
    fs::create_directories(internalSessionDir);

    std::ostringstream manifest;
    for (const auto& rel : touchedFiles(plan)) {
        fs::path source = c.root / rel;
        if (fs::exists(source) && fs::is_regular_file(source)) {
            fs::path publicBackupPath = publicBackupSessionDir / rel;
            fs::path internalBackupPath = internalSessionDir / rel;
            fs::create_directories(publicBackupPath.parent_path());
            fs::create_directories(internalBackupPath.parent_path());
            fs::copy_file(source, publicBackupPath, fs::copy_options::overwrite_existing);
            fs::copy_file(source, internalBackupPath, fs::copy_options::overwrite_existing);
            manifest << "BACKUP " << rel << "\n";
            ui.fileStatus(rel, "backed up");
            ui.log("Backup created: " + publicBackupPath.string());
        } else {
            manifest << "CREATED_OR_MISSING " << rel << "\n";
            ui.fileStatus(rel, "new file or missing before change");
        }
    }

    writeFile(internalSessionDir / "manifest.txt", manifest.str());
    writeFile(aiDir(c) / "last-session.txt", session);
}

static void saveLog(const AppConfig& c, const std::string& session, const ChangePlan& plan) {
    fs::create_directories(aiDir(c));
    std::ofstream log(aiDir(c) / "ai-edit.log", std::ios::app);
    log << "\nSESSION " << session << "\n";
    log << "BACKUP_ROOT: " << c.backupRoot.string() << "\n";
    log << "REQUEST: " << c.request << "\n";
    log << "SUMMARY: " << plan.summary << "\n";
    log << "FILES:\n";
    for (const auto& ch : plan.changes) {
        log << "- " << ch.action << " " << ch.path;
        if (!ch.newPath.empty()) {
            log << " -> " << ch.newPath;
        }
        log << "\n";
    }
}

static void runSyntaxChecks(const AppConfig& c, const ChangePlan& plan, const UiHooks& ui) {
    if (c.noSyntaxCheck) {
        ui.log("Syntax checks disabled.");
        return;
    }

    for (const auto& rel : touchedFiles(plan)) {
        fs::path full = c.root / rel;
        if (!fs::exists(full) || !fs::is_regular_file(full)) {
            continue;
        }

        std::string out;
        int rc = 0;
        const std::string ext = full.extension().string();
        if (ext == ".php") {
            ui.fileStatus(rel, "PHP syntax check running");
            rc = runProcessCapture({"php", "-l", full.string()}, c.root, &out);
        } else if (ext == ".py") {
            ui.fileStatus(rel, "Python syntax check running");
            rc = runProcessCapture({"python3", "-m", "py_compile", full.string()}, c.root, &out);
        } else if (ext == ".js" || ext == ".mjs" || ext == ".cjs") {
            ui.fileStatus(rel, "Node syntax check running");
            rc = runProcessCapture({"node", "--check", full.string()}, c.root, &out);
        } else if (ext == ".json") {
            ui.fileStatus(rel, "JSON syntax check running");
            try {
                json::parse(readFile(full));
                rc = 0;
            } catch (const std::exception& e) {
                out = e.what();
                rc = 1;
            }
        } else {
            ui.fileStatus(rel, "applied, syntax check not configured");
            continue;
        }

        if (!out.empty()) {
            ui.log(out);
        }
        if (rc != 0) {
            ui.fileStatus(rel, "syntax check failed");
            throw std::runtime_error("Syntax check failed for " + rel);
        }
        ui.fileStatus(rel, "syntax OK");
    }
}

static void applyChangePlanCore(const AppConfig& c, const ChangePlan& plan, const UiHooks& ui) {
    validateChangePlan(c, plan);

    for (const auto& ch : plan.changes) {
        if (ch.action == "delete" && !c.allowDelete) {
            throw std::runtime_error("Change plan includes deletion. Toggle Allow Delete before applying.");
        }
        if (ch.action == "rename" && !c.allowRename) {
            throw std::runtime_error("Change plan includes rename. Toggle Allow Rename before applying.");
        }
    }

    for (const auto& ch : plan.changes) {
        fs::path target = c.root / ch.path;

        if (ch.action == "delete") {
            if (fs::exists(target)) {
                fs::remove(target);
                ui.fileStatus(ch.path, "deleted");
                ui.log("Deleted " + ch.path);
            }
            continue;
        }

        if (ch.action == "rename") {
            fs::path dest = c.root / ch.newPath;
            fs::create_directories(dest.parent_path());
            if (!fs::exists(target)) {
                throw std::runtime_error("Cannot rename missing file: " + ch.path);
            }
            fs::rename(target, dest);
            ui.fileStatus(ch.path, "renamed");
            ui.fileStatus(ch.newPath, "renamed target");
            ui.log("Renamed " + ch.path + " to " + ch.newPath);
            if (!ch.content.empty()) {
                writeFile(dest, ch.content);
                ui.fileStatus(ch.newPath, "written after rename");
            }
            continue;
        }

        if (ch.action == "write" || ch.action == "create") {
            writeFile(target, ch.content);
            ui.fileStatus(ch.path, ch.action == "create" ? "created" : "written");
            ui.log(std::string(ch.action == "create" ? "Created " : "Wrote ") + ch.path);
            continue;
        }
    }

    runSyntaxChecks(c, plan, ui);
}

static void rollbackLast(const AppConfig& c, const UiHooks& ui) {
    fs::path lastFile = aiDir(c) / "last-session.txt";
    if (!fs::exists(lastFile)) {
        throw std::runtime_error("No last session file found. Nothing to roll back.");
    }

    std::string session = readFile(lastFile);
    while (!session.empty() && (session.back() == '\n' || session.back() == '\r')) {
        session.pop_back();
    }
    if (session.empty()) {
        throw std::runtime_error("Last session file is empty.");
    }

    fs::path bdir = aiDir(c) / "backups" / session;
    fs::path manifestPath = bdir / "manifest.txt";
    if (!fs::exists(manifestPath)) {
        throw std::runtime_error("Backup manifest is missing for session: " + session);
    }

    std::istringstream manifest(readFile(manifestPath));
    std::string type;
    std::string rel;
    while (manifest >> type) {
        std::getline(manifest, rel);
        if (!rel.empty() && rel[0] == ' ') {
            rel.erase(rel.begin());
        }
        if (rel.empty() || pathLooksUnsafe(rel)) {
            continue;
        }

        fs::path target = c.root / rel;
        if (type == "BACKUP") {
            fs::path backup = bdir / rel;
            if (fs::exists(backup) && fs::is_regular_file(backup)) {
                fs::create_directories(target.parent_path());
                fs::copy_file(backup, target, fs::copy_options::overwrite_existing);
                ui.fileStatus(rel, "restored");
                ui.log("Restored " + rel);
            }
        } else if (type == "CREATED_OR_MISSING") {
            if (fs::exists(target) && fs::is_regular_file(target)) {
                fs::remove(target);
                ui.fileStatus(rel, "removed during rollback");
                ui.log("Removed created file " + rel);
            }
        }
    }
    ui.log("Rollback complete.");
}

static std::string trimToWidth(const std::string& s, int width) {
    if (width <= 0) {
        return "";
    }
    if (static_cast<int>(s.size()) <= width) {
        return s;
    }
    if (width <= 3) {
        return s.substr(0, static_cast<size_t>(width));
    }
    return s.substr(0, static_cast<size_t>(width - 3)) + "...";
}

static std::vector<std::string> wrapText(const std::string& text, int width) {
    std::vector<std::string> out;
    if (width <= 1) {
        out.push_back("");
        return out;
    }

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            out.push_back("");
            continue;
        }
        size_t pos = 0;
        while (pos < line.size()) {
            out.push_back(line.substr(pos, static_cast<size_t>(width)));
            pos += static_cast<size_t>(width);
        }
    }
    if (out.empty()) {
        out.push_back("");
    }
    return out;
}

enum class FocusPane {
    Files,
    Preview,
    Log
};

struct State {
    AppConfig config;
    std::vector<fs::path> files;
    std::map<std::string, std::string> fileStatuses;
    int selectedFile = 0;
    std::string fileView;
    std::string previewText = "No change plan yet. Press Ctrl+P to edit the request, then Ctrl+G to generate.";
    std::string logText = "AI Edit TUI started.\n";
    std::string fullPromptText;
    std::string rawResponseText;
    std::string statusLine = "Ready";
    ChangePlan plan;
    bool hasPlan = false;
    bool working = false;
    bool quit = false;
    FocusPane focus = FocusPane::Files;
    int fileScroll = 0;
    int previewScroll = 0;
    int logScroll = 0;
};

struct SharedState {
    State state;
    std::mutex m;
};

static UiHooks makeHooks(SharedState& shared) {
    UiHooks ui;
    ui.log = [&shared](const std::string& s) {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.logText += s;
        if (shared.state.logText.empty() || shared.state.logText.back() != '\n') {
            shared.state.logText += "\n";
        }
    };
    ui.changes = [&shared](const std::string& s) {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.previewText = s;
        shared.state.previewScroll = 0;
    };
    ui.prompt = [&shared](const std::string& s) {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.fullPromptText = s;
    };
    ui.response = [&shared](const std::string& s) {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.rawResponseText = s;
    };
    ui.fileStatus = [&shared](const std::string& file, const std::string& status) {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.fileStatuses[file] = status;
    };
    return ui;
}

static void setStatus(SharedState& shared, const std::string& status) {
    std::lock_guard<std::mutex> lock(shared.m);
    shared.state.statusLine = status;
}

static AppConfig copyConfig(SharedState& shared) {
    std::lock_guard<std::mutex> lock(shared.m);
    return shared.state.config;
}

static void rescanFilesAsync(SharedState& shared) {
    {
        std::lock_guard<std::mutex> lock(shared.m);
        if (shared.state.working) {
            return;
        }
        shared.state.working = true;
        shared.state.statusLine = "Scanning project files";
        shared.state.fileStatuses.clear();
    }

    std::thread([&shared]() {
        UiHooks ui = makeHooks(shared);
        try {
            AppConfig c = copyConfig(shared);
            auto files = listProjectFiles(c, ui);
            {
                std::lock_guard<std::mutex> lock(shared.m);
                shared.state.files = files;
                if (shared.state.selectedFile >= static_cast<int>(shared.state.files.size())) {
                    shared.state.selectedFile = std::max(0, static_cast<int>(shared.state.files.size()) - 1);
                }
                shared.state.working = false;
                shared.state.statusLine = "Scan complete";
            }
        } catch (const std::exception& e) {
            ui.log(std::string("Scan failed: ") + e.what());
            std::lock_guard<std::mutex> lock(shared.m);
            shared.state.working = false;
            shared.state.statusLine = "Scan failed";
        }
    }).detach();
}

static void generatePlanAsync(SharedState& shared) {
    {
        std::lock_guard<std::mutex> lock(shared.m);
        if (shared.state.working) {
            return;
        }
        if (shared.state.config.request.empty()) {
            shared.state.statusLine = "No request set. Press Ctrl+P first.";
            return;
        }
        shared.state.working = true;
        shared.state.hasPlan = false;
        shared.state.previewText = "Working. Building project context and asking OpenAI...";
        shared.state.previewScroll = 0;
        shared.state.statusLine = "Generating change plan";
        shared.state.fileStatuses.clear();
    }

    std::thread([&shared]() {
        UiHooks ui = makeHooks(shared);
        try {
            AppConfig c = copyConfig(shared);
            auto files = listProjectFiles(c, ui);
            {
                std::lock_guard<std::mutex> lock(shared.m);
                shared.state.files = files;
            }
            std::string ctx = buildProjectContext(c, files, ui);
            std::string outputText = callOpenAI(c, ctx, ui);
            ChangePlan plan = parseChangePlan(outputText);
            validateChangePlan(c, plan);
            std::string preview = renderChangePreview(c, plan);
            saveChangePlan(c, plan);
            {
                std::lock_guard<std::mutex> lock(shared.m);
                shared.state.plan = plan;
                shared.state.hasPlan = true;
                shared.state.previewText = preview;
                shared.state.previewScroll = 0;
                shared.state.statusLine = "Change plan ready. Press Ctrl+A to apply.";
                shared.state.working = false;
            }
        } catch (const std::exception& e) {
            ui.log(std::string("Generate failed: ") + e.what());
            std::lock_guard<std::mutex> lock(shared.m);
            shared.state.previewText += std::string("\n\nERROR: ") + e.what() + "\n";
            shared.state.statusLine = "Generate failed";
            shared.state.working = false;
        }
    }).detach();
}

static void applyPlanAsync(SharedState& shared) {
    ChangePlan plan;
    AppConfig c;
    {
        std::lock_guard<std::mutex> lock(shared.m);
        if (shared.state.working) {
            return;
        }
        if (!shared.state.hasPlan) {
            shared.state.statusLine = "No change plan to apply.";
            return;
        }
        shared.state.working = true;
        shared.state.statusLine = "Applying change plan";
        plan = shared.state.plan;
        c = shared.state.config;
    }

    std::thread([&shared, c, plan]() {
        UiHooks ui = makeHooks(shared);
        try {
            const std::string session = nowStamp();
            autoBackupTouchedFiles(c, plan, session, ui);
            applyChangePlanCore(c, plan, ui);
            saveLog(c, session, plan);
            auto files = listProjectFiles(c, ui);
            {
                std::lock_guard<std::mutex> lock(shared.m);
                shared.state.files = files;
                shared.state.working = false;
                shared.state.statusLine = "Applied successfully. Ctrl+R can roll back the last session.";
            }
        } catch (const std::exception& e) {
            ui.log(std::string("Apply failed: ") + e.what());
            std::lock_guard<std::mutex> lock(shared.m);
            shared.state.working = false;
            shared.state.statusLine = "Apply failed. Review log, then rollback if needed.";
        }
    }).detach();
}

static void rollbackAsync(SharedState& shared) {
    AppConfig c;
    {
        std::lock_guard<std::mutex> lock(shared.m);
        if (shared.state.working) {
            return;
        }
        shared.state.working = true;
        shared.state.statusLine = "Rolling back last session";
        c = shared.state.config;
    }

    std::thread([&shared, c]() {
        UiHooks ui = makeHooks(shared);
        try {
            rollbackLast(c, ui);
            auto files = listProjectFiles(c, ui);
            {
                std::lock_guard<std::mutex> lock(shared.m);
                shared.state.files = files;
                shared.state.working = false;
                shared.state.statusLine = "Rollback complete";
            }
        } catch (const std::exception& e) {
            ui.log(std::string("Rollback failed: ") + e.what());
            std::lock_guard<std::mutex> lock(shared.m);
            shared.state.working = false;
            shared.state.statusLine = "Rollback failed";
        }
    }).detach();
}

static void openExternalEditor(const fs::path& path) {
    const char* ed = std::getenv("EDITOR");
    std::string editor = (ed && *ed) ? ed : "nano";
    std::string cmd = editor + " \"" + path.string() + "\"";
    def_prog_mode();
    endwin();
    std::system(cmd.c_str());
    reset_prog_mode();
    refresh();
}

static void editRequest(SharedState& shared) {
    AppConfig c = copyConfig(shared);
    fs::create_directories(aiDir(c));
    fs::path requestFile = aiDir(c) / "request.txt";
    if (!fs::exists(requestFile)) {
        writeFile(requestFile, c.request.empty() ? "Describe the code change you want here.\n" : c.request);
    }
    openExternalEditor(requestFile);
    std::string request = readFile(requestFile);
    {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.config.request = request;
        shared.state.statusLine = "Request loaded from .ai-edit/request.txt";
    }
}

static void openSelectedFile(SharedState& shared) {
    fs::path full;
    {
        std::lock_guard<std::mutex> lock(shared.m);
        if (shared.state.files.empty()) {
            shared.state.statusLine = "No file selected.";
            return;
        }
        int idx = std::clamp(shared.state.selectedFile, 0, static_cast<int>(shared.state.files.size()) - 1);
        full = shared.state.config.root / shared.state.files[static_cast<size_t>(idx)];
    }
    openExternalEditor(full);
}

static void loadSelectedFileView(SharedState& shared) {
    try {
        std::lock_guard<std::mutex> lock(shared.m);
        if (shared.state.files.empty()) {
            shared.state.fileView.clear();
            return;
        }
        int idx = std::clamp(shared.state.selectedFile, 0, static_cast<int>(shared.state.files.size()) - 1);
        fs::path full = shared.state.config.root / shared.state.files[static_cast<size_t>(idx)];
        shared.state.fileView = readFile(full);
        shared.state.previewText = "FILE: " + shared.state.files[static_cast<size_t>(idx)].string() + "\n\n" + shared.state.fileView;
        shared.state.previewScroll = 0;
        shared.state.statusLine = "Viewing file. Press Ctrl+G to generate AI edits.";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.statusLine = std::string("Failed to load file: ") + e.what();
    }
}

static void drawBoxTitle(WINDOW* win, const std::string& title, bool focused) {
    box(win, 0, 0);
    if (focused) {
        wattron(win, A_BOLD);
    }
    mvwprintw(win, 0, 2, " %s ", title.c_str());
    if (focused) {
        wattroff(win, A_BOLD);
    }
}

static void drawFilePane(WINDOW* win, const State& s, int height, int width) {
    drawBoxTitle(win, "Files", s.focus == FocusPane::Files);
    int innerH = height - 2;
    int start = std::max(0, s.fileScroll);
    for (int row = 0; row < innerH; ++row) {
        int idx = start + row;
        if (idx >= static_cast<int>(s.files.size())) {
            break;
        }
        std::string rel = s.files[static_cast<size_t>(idx)].string();
        std::string status;
        auto it = s.fileStatuses.find(rel);
        if (it != s.fileStatuses.end()) {
            status = " [" + it->second + "]";
        }
        std::string line = trimToWidth(rel + status, width - 3);
        if (idx == s.selectedFile) {
            wattron(win, A_REVERSE);
        }
        mvwprintw(win, row + 1, 1, "%*s", width - 2, "");
        mvwprintw(win, row + 1, 1, "%s", line.c_str());
        if (idx == s.selectedFile) {
            wattroff(win, A_REVERSE);
        }
    }
}

static void drawTextPane(WINDOW* win, const std::string& title, const std::string& text, int scroll, bool focused, int height, int width) {
    drawBoxTitle(win, title, focused);
    auto lines = wrapText(text, width - 3);
    int innerH = height - 2;
    for (int row = 0; row < innerH; ++row) {
        int idx = scroll + row;
        if (idx >= static_cast<int>(lines.size())) {
            break;
        }
        mvwprintw(win, row + 1, 1, "%s", trimToWidth(lines[static_cast<size_t>(idx)], width - 3).c_str());
    }
    std::string pos = std::to_string(std::min(scroll + 1, std::max(1, static_cast<int>(lines.size())))) + "/" + std::to_string(lines.size());
    mvwprintw(win, 0, std::max(1, width - static_cast<int>(pos.size()) - 2), "%s", pos.c_str());
}

static void drawUi(SharedState& shared) {
    State s;
    {
        std::lock_guard<std::mutex> lock(shared.m);
        s = shared.state;
    }

    int h = 0;
    int w = 0;
    getmaxyx(stdscr, h, w);
    erase();

    if (h < 18 || w < 70) {
        mvprintw(0, 0, "Terminal too small. Need at least 70x18.");
        refresh();
        return;
    }

    attron(A_BOLD);
    mvprintw(0, 0, " AI Edit TUI ");
    attroff(A_BOLD);
    mvprintw(0, 14, "Root: %s", trimToWidth(s.config.root.string(), w - 60).c_str());
    mvprintw(0, w - 38, "Model: %s", trimToWidth(s.config.model, 25).c_str());
    if (s.working) {
        attron(A_BLINK);
        mvprintw(0, w - 10, "WORKING");
        attroff(A_BLINK);
    }

    int helpY = h - 3;
    int statusY = h - 2;
    int mainH = h - 4;
    int leftW = std::min(42, std::max(26, w / 3));
    int rightW = w - leftW;
    int upperH = std::max(6, (mainH * 2) / 3);
    int lowerH = mainH - upperH;

    WINDOW* files = newwin(mainH, leftW, 1, 0);
    WINDOW* preview = newwin(upperH, rightW, 1, leftW);
    WINDOW* log = newwin(lowerH, rightW, 1 + upperH, leftW);

    drawFilePane(files, s, mainH, leftW);
    drawTextPane(preview, "Preview / File View", s.previewText, s.previewScroll, s.focus == FocusPane::Preview, upperH, rightW);
    drawTextPane(log, "Log", s.logText, s.logScroll, s.focus == FocusPane::Log, lowerH, rightW);

    wrefresh(files);
    wrefresh(preview);
    wrefresh(log);
    delwin(files);
    delwin(preview);
    delwin(log);

    mvhline(helpY - 1, 0, ACS_HLINE, w);
    mvprintw(helpY, 0, "Tab focus | Enter view | Ctrl+P request | Ctrl+G generate | Ctrl+A apply | Ctrl+R rollback | Ctrl+O edit file | Ctrl+F rescan | d delete:%s | n rename:%s | s syntax:%s | q quit",
             s.config.allowDelete ? "on" : "off",
             s.config.allowRename ? "on" : "off",
             s.config.noSyntaxCheck ? "off" : "on");
    mvprintw(statusY, 0, "Status: %s", trimToWidth(s.statusLine, w - 9).c_str());
    mvprintw(h - 1, 0, "Request: %s", trimToWidth(s.config.request.empty() ? "not set" : s.config.request, w - 10).c_str());
    refresh();
}

static void clampScrolls(SharedState& shared) {
    std::lock_guard<std::mutex> lock(shared.m);
    if (shared.state.selectedFile < 0) {
        shared.state.selectedFile = 0;
    }
    if (shared.state.selectedFile >= static_cast<int>(shared.state.files.size())) {
        shared.state.selectedFile = std::max(0, static_cast<int>(shared.state.files.size()) - 1);
    }
    int h = 0, w = 0;
    getmaxyx(stdscr, h, w);
    int mainH = h - 4;
    int leftW = std::min(42, std::max(26, w / 3));
    (void)leftW;
    int visibleFiles = std::max(1, mainH - 2);
    if (shared.state.selectedFile < shared.state.fileScroll) {
        shared.state.fileScroll = shared.state.selectedFile;
    }
    if (shared.state.selectedFile >= shared.state.fileScroll + visibleFiles) {
        shared.state.fileScroll = shared.state.selectedFile - visibleFiles + 1;
    }
    shared.state.fileScroll = std::max(0, shared.state.fileScroll);
}

static void cycleFocus(SharedState& shared) {
    std::lock_guard<std::mutex> lock(shared.m);
    if (shared.state.focus == FocusPane::Files) {
        shared.state.focus = FocusPane::Preview;
    } else if (shared.state.focus == FocusPane::Preview) {
        shared.state.focus = FocusPane::Log;
    } else {
        shared.state.focus = FocusPane::Files;
    }
}

static void handleKey(SharedState& shared, int ch) {
    if (ch == 'q' || ch == 27) {
        std::lock_guard<std::mutex> lock(shared.m);
        if (!shared.state.working) {
            shared.state.quit = true;
        } else {
            shared.state.statusLine = "Worker is active. Wait for it to finish before quitting.";
        }
        return;
    }

    if (ch == '\t') {
        cycleFocus(shared);
        return;
    }

    if (ch == 16) { // Ctrl+P
        editRequest(shared);
        return;
    }
    if (ch == 7) { // Ctrl+G
        generatePlanAsync(shared);
        return;
    }
    if (ch == 1) { // Ctrl+A
        applyPlanAsync(shared);
        return;
    }
    if (ch == 18) { // Ctrl+R
        rollbackAsync(shared);
        return;
    }
    if (ch == 6) { // Ctrl+F
        rescanFilesAsync(shared);
        return;
    }
    if (ch == 15) { // Ctrl+O
        openSelectedFile(shared);
        return;
    }
    if (ch == 'd') {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.config.allowDelete = !shared.state.config.allowDelete;
        shared.state.statusLine = shared.state.config.allowDelete ? "Delete actions allowed" : "Delete actions blocked";
        return;
    }
    if (ch == 'n') {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.config.allowRename = !shared.state.config.allowRename;
        shared.state.statusLine = shared.state.config.allowRename ? "Rename actions allowed" : "Rename actions blocked";
        return;
    }
    if (ch == 's') {
        std::lock_guard<std::mutex> lock(shared.m);
        shared.state.config.noSyntaxCheck = !shared.state.config.noSyntaxCheck;
        shared.state.statusLine = shared.state.config.noSyntaxCheck ? "Syntax checks disabled" : "Syntax checks enabled";
        return;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        loadSelectedFileView(shared);
        return;
    }

    std::lock_guard<std::mutex> lock(shared.m);
    int step = 1;
    if (ch == KEY_NPAGE) {
        step = 10;
        ch = KEY_DOWN;
    } else if (ch == KEY_PPAGE) {
        step = 10;
        ch = KEY_UP;
    }

    if (shared.state.focus == FocusPane::Files) {
        if (ch == KEY_UP) {
            shared.state.selectedFile -= step;
        } else if (ch == KEY_DOWN) {
            shared.state.selectedFile += step;
        }
    } else if (shared.state.focus == FocusPane::Preview) {
        if (ch == KEY_UP) {
            shared.state.previewScroll = std::max(0, shared.state.previewScroll - step);
        } else if (ch == KEY_DOWN) {
            shared.state.previewScroll += step;
        }
    } else if (shared.state.focus == FocusPane::Log) {
        if (ch == KEY_UP) {
            shared.state.logScroll = std::max(0, shared.state.logScroll - step);
        } else if (ch == KEY_DOWN) {
            shared.state.logScroll += step;
        }
    }
}

static void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [project-root] [--model model] [--backup-root path]\n";
    std::cerr << "Environment: OPENAI_API_KEY is required. AI_EDIT_MODEL can override the default model.\n";
}

int main(int argc, char** argv) {
    AppConfig config;
    config.root = fs::current_path();

    const char* envModel = std::getenv("AI_EDIT_MODEL");
    if (envModel && *envModel) {
        config.model = envModel;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--model" && i + 1 < argc) {
            config.model = argv[++i];
            continue;
        }
        if (arg == "--backup-root" && i + 1 < argc) {
            config.backupRoot = argv[++i];
            continue;
        }
        if (arg == "--no-syntax-check") {
            config.noSyntaxCheck = true;
            continue;
        }
        config.root = arg;
    }

    try {
        config.root = fs::weakly_canonical(config.root);
        if (!fs::exists(config.root) || !fs::is_directory(config.root)) {
            throw std::runtime_error("Project root is not a directory: " + config.root.string());
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    SharedState shared;
    shared.state.config = config;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    rescanFilesAsync(shared);

    while (true) {
        drawUi(shared);
        int ch = getch();
        if (ch != ERR) {
            handleKey(shared, ch);
            clampScrolls(shared);
        }
        {
            std::lock_guard<std::mutex> lock(shared.m);
            if (shared.state.quit) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    endwin();
    curl_global_cleanup();
    return 0;
}
