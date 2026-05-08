/*
  ai-edit-gui.cpp
  Native Linux GUI version of AI Edit, no Git dependency.

  What this app does:
    - Provides a three-pane Linux desktop interface.
    - Left pane has a mouse-selectable directory tree and live file status.
    - Main upper pane shows live code changes for each file.
    - Bottom pane contains the user prompt, full OpenAI prompt, raw OpenAI response, and run log.
    - OpenAI work runs in the background so the GUI remains responsive.
    - File changes are applied directly by this program, without Git.
    - Backups are created before edits.
    - Rollback restores the last applied AI edit.

  Ubuntu/Debian dependencies:
    sudo apt update
    sudo apt install build-essential pkg-config qt6-base-dev libcurl4-openssl-dev nlohmann-json3-dev php-cli python3 nodejs

  Build:
    g++ -std=c++20 ai-edit-gui.cpp -o ai-edit-gui $(pkg-config --cflags --libs Qt6Widgets) -lcurl

  Run:
    export OPENAI_API_KEY="your_api_key_here"
    ./ai-edit-gui
*/

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QThread>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

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
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02d_%02d-%02d-%02d",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
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
    std::string r = canonicalSafe(root).string();
    std::string t = canonicalSafe(target).string();
    std::string rootCanonical = canonicalSafe(root).string();

    if (!r.empty() && r.back() != '/') {
        r += "/";
    }
    return t == rootCanonical || startsWith(t, r);
}

static fs::path aiDir(const AppConfig& c) {
    return c.root / ".ai-edit";
}

static bool shouldSkipDir(const fs::path& p) {
    static const std::set<std::string> skip = {
        ".ai-edit", "node_modules", "vendor", "cache", "tmp", "logs", "uploads",
        "backup", "backups", ".idea", ".vscode", "dist", "build", "target", "__pycache__"
    };
    return skip.count(p.filename().string()) > 0;
}

static bool allowedExt(const fs::path& p) {
    static const std::set<std::string> exts = {
        ".php", ".inc", ".html", ".htm", ".css", ".js", ".jsx", ".ts", ".tsx",
        ".json", ".sql", ".txt", ".md", ".xml", ".yml", ".yaml", ".conf",
        ".ini", ".cpp", ".c", ".h", ".hpp", ".py", ".sh", ".rb", ".go",
        ".rs", ".java", ".cs", ".env.example", ".dockerfile"
    };

    std::string name = p.filename().string();
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

static int runProcessCapture(
    const std::vector<std::string>& args,
    const fs::path& cwd,
    std::string* output
) {
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
        "Do not modify files outside PROJECT_ROOT. "
        "Do not touch .env files. "
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ai-edit-gui-no-vcs/3.0");

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
        return "  No text changes.\n";
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
            throw std::runtime_error("Change plan includes deletion. Enable Allow delete before applying.");
        }
        if (ch.action == "rename" && !c.allowRename) {
            throw std::runtime_error("Change plan includes rename. Enable Allow rename before applying.");
        }
    }

    const std::string session = nowStamp();
    fs::create_directories(aiDir(c));
    fs::create_directories(c.backupRoot);

    autoBackupTouchedFiles(c, plan, session, ui);

    ui.log("Applying file changes directly...");
    for (const auto& ch : plan.changes) {
        if (ch.action == "write" || ch.action == "create") {
            fs::path target = c.root / ch.path;
            writeFile(target, ch.content);
            ui.fileStatus(ch.path, ch.action == "create" ? "created" : "written");
        } else if (ch.action == "delete") {
            fs::path target = c.root / ch.path;
            if (fs::exists(target) && fs::is_regular_file(target)) {
                fs::remove(target);
                ui.fileStatus(ch.path, "deleted");
            } else {
                ui.fileStatus(ch.path, "delete skipped, file missing");
            }
        } else if (ch.action == "rename") {
            fs::path oldPath = c.root / ch.path;
            fs::path newPath = c.root / ch.newPath;
            if (!fs::exists(oldPath)) {
                throw std::runtime_error("Cannot rename missing file: " + ch.path);
            }
            fs::create_directories(newPath.parent_path());
            fs::rename(oldPath, newPath);
            ui.fileStatus(ch.path, "renamed from here");
            ui.fileStatus(ch.newPath, "renamed to here");
            if (!ch.content.empty()) {
                writeFile(newPath, ch.content);
                ui.fileStatus(ch.newPath, "renamed and written");
            }
        }
    }

    runSyntaxChecks(c, plan, ui);
    saveChangePlan(c, plan);
    saveLog(c, session, plan);

    ui.log("Applied successfully.");
    ui.log("Changed-file backups saved in: " + (c.backupRoot / session).string());
    ui.log("Rollback available from the Rollback Last button.");
}

static void rollbackLastCore(const AppConfig& c, const UiHooks& ui) {
    fs::path sessionFile = aiDir(c) / "last-session.txt";
    if (!fs::exists(sessionFile)) {
        throw std::runtime_error("No rollback session found.");
    }

    std::string session = readFile(sessionFile);
    session.erase(std::remove(session.begin(), session.end(), '\n'), session.end());
    session.erase(std::remove(session.begin(), session.end(), '\r'), session.end());

    fs::path bdir = aiDir(c) / "backups" / session;
    fs::path manifest = bdir / "manifest.txt";
    if (!fs::exists(manifest)) {
        throw std::runtime_error("Rollback manifest missing.");
    }

    ui.log("Rolling back session: " + session);

    std::istringstream in(readFile(manifest));
    std::string type;
    std::string rel;
    while (in >> type >> rel) {
        fs::path target = c.root / rel;
        fs::path backup = bdir / rel;

        if (type == "BACKUP" && fs::exists(backup)) {
            fs::create_directories(target.parent_path());
            fs::copy_file(backup, target, fs::copy_options::overwrite_existing);
            ui.fileStatus(rel, "restored");
            ui.log("Restored " + rel);
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

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("AI Edit GUI, Direct Linux Project Editor");
        resize(1550, 980);

        QWidget* central = new QWidget(this);
        QVBoxLayout* rootLayout = new QVBoxLayout(central);
        rootLayout->setContentsMargins(10, 10, 10, 10);
        rootLayout->setSpacing(8);

        QGroupBox* configBox = new QGroupBox("Project and OpenAI Settings", central);
        QGridLayout* configLayout = new QGridLayout(configBox);

        rootEdit = new QLineEdit(configBox);
        backupEdit = new QLineEdit("/backup", configBox);
        modelEdit = new QLineEdit("gpt-4.1", configBox);

        maxProjectSpin = new QSpinBox(configBox);
        maxProjectSpin->setRange(10000, 2000000);
        maxProjectSpin->setSingleStep(10000);
        maxProjectSpin->setValue(220000);
        maxProjectSpin->setSuffix(" bytes");

        maxFileSpin = new QSpinBox(configBox);
        maxFileSpin->setRange(1000, 500000);
        maxFileSpin->setSingleStep(1000);
        maxFileSpin->setValue(60000);
        maxFileSpin->setSuffix(" bytes");

        allowDeleteBox = new QCheckBox("Allow delete", configBox);
        allowRenameBox = new QCheckBox("Allow rename", configBox);
        noSyntaxBox = new QCheckBox("Skip syntax checks", configBox);

        QPushButton* browseRoot = new QPushButton("Browse", configBox);
        QPushButton* browseBackup = new QPushButton("Browse", configBox);

        generateButton = new QPushButton("Generate Changes", configBox);
        applyButton = new QPushButton("Apply Displayed Changes", configBox);
        rollbackButton = new QPushButton("Rollback Last", configBox);
        applyButton->setEnabled(false);

        progress = new QProgressBar(configBox);
        progress->setRange(0, 0);
        progress->setVisible(false);

        configLayout->addWidget(new QLabel("Project root:"), 0, 0);
        configLayout->addWidget(rootEdit, 0, 1, 1, 4);
        configLayout->addWidget(browseRoot, 0, 5);

        configLayout->addWidget(new QLabel("Backup root:"), 1, 0);
        configLayout->addWidget(backupEdit, 1, 1, 1, 4);
        configLayout->addWidget(browseBackup, 1, 5);

        configLayout->addWidget(new QLabel("Model:"), 2, 0);
        configLayout->addWidget(modelEdit, 2, 1);
        configLayout->addWidget(new QLabel("Max project:"), 2, 2);
        configLayout->addWidget(maxProjectSpin, 2, 3);
        configLayout->addWidget(new QLabel("Max file:"), 2, 4);
        configLayout->addWidget(maxFileSpin, 2, 5);

        configLayout->addWidget(allowDeleteBox, 3, 0);
        configLayout->addWidget(allowRenameBox, 3, 1);
        configLayout->addWidget(noSyntaxBox, 3, 2);
        configLayout->addWidget(generateButton, 3, 3);
        configLayout->addWidget(applyButton, 3, 4);
        configLayout->addWidget(rollbackButton, 3, 5);
        configLayout->addWidget(progress, 4, 0, 1, 6);

        rootLayout->addWidget(configBox);

        mainSplitter = new QSplitter(Qt::Horizontal, central);

        QWidget* sidePane = new QWidget(mainSplitter);
        QVBoxLayout* sideLayout = new QVBoxLayout(sidePane);
        sideLayout->setContentsMargins(0, 0, 0, 0);
        sideLayout->setSpacing(6);

        QLabel* dirLabel = new QLabel("Directory Browser, click a folder to select project root", sidePane);
        sideLayout->addWidget(dirLabel);

        dirModel = new QFileSystemModel(sidePane);
        dirModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
        dirModel->setRootPath(QDir::rootPath());

        dirTree = new QTreeView(sidePane);
        dirTree->setModel(dirModel);
        dirTree->setRootIndex(dirModel->index(QDir::homePath()));
        dirTree->setAnimated(true);
        dirTree->setIndentation(16);
        dirTree->setSortingEnabled(true);
        dirTree->sortByColumn(0, Qt::AscendingOrder);
        dirTree->setHeaderHidden(false);
        dirTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int i = 1; i < dirModel->columnCount(); ++i) {
            dirTree->hideColumn(i);
        }
        sideLayout->addWidget(dirTree, 3);

        QPushButton* useSelectedDirButton = new QPushButton("Use Selected Folder", sidePane);
        sideLayout->addWidget(useSelectedDirButton);

        QLabel* statusLabel = new QLabel("Live File Status", sidePane);
        sideLayout->addWidget(statusLabel);

        fileStatusList = new QListWidget(sidePane);
        fileStatusList->setMinimumWidth(340);
        sideLayout->addWidget(fileStatusList, 2);

        rightSplitter = new QSplitter(Qt::Vertical, mainSplitter);

        changeView = new QPlainTextEdit(rightSplitter);
        changeView->setReadOnly(true);
        changeView->setLineWrapMode(QPlainTextEdit::NoWrap);
        changeView->setPlaceholderText("Generated file changes and per-file previews will appear here.");

        bottomTabs = new QTabWidget(rightSplitter);

        requestEdit = new QPlainTextEdit(bottomTabs);
        requestEdit->setPlaceholderText("Describe the change you want OpenAI to make to the selected project.");

        fullPromptView = new QPlainTextEdit(bottomTabs);
        fullPromptView->setReadOnly(true);
        fullPromptView->setLineWrapMode(QPlainTextEdit::NoWrap);

        responseView = new QPlainTextEdit(bottomTabs);
        responseView->setReadOnly(true);
        responseView->setLineWrapMode(QPlainTextEdit::NoWrap);

        logView = new QPlainTextEdit(bottomTabs);
        logView->setReadOnly(true);
        logView->setLineWrapMode(QPlainTextEdit::NoWrap);

        bottomTabs->addTab(requestEdit, "User Prompt");
        bottomTabs->addTab(fullPromptView, "Prompt Sent to OpenAI");
        bottomTabs->addTab(responseView, "OpenAI Response");
        bottomTabs->addTab(logView, "Run Log");

        rightSplitter->addWidget(changeView);
        rightSplitter->addWidget(bottomTabs);
        rightSplitter->setStretchFactor(0, 4);
        rightSplitter->setStretchFactor(1, 1);
        rightSplitter->setSizes({680, 260});

        mainSplitter->addWidget(sidePane);
        mainSplitter->addWidget(rightSplitter);
        mainSplitter->setStretchFactor(0, 1);
        mainSplitter->setStretchFactor(1, 4);
        mainSplitter->setSizes({390, 1160});

        rootLayout->addWidget(mainSplitter, 1);
        setCentralWidget(central);

        statusBar()->showMessage("Ready");
        installMonoFonts();
        restoreSettings();
        syncDirectoryTreeToRootEdit();

        connect(browseRoot, &QPushButton::clicked, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(this, "Select Project Root", rootEdit->text());
            if (!dir.isEmpty()) {
                rootEdit->setText(dir);
                syncDirectoryTreeToRootEdit();
            }
        });

        connect(browseBackup, &QPushButton::clicked, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(this, "Select Backup Root", backupEdit->text());
            if (!dir.isEmpty()) {
                backupEdit->setText(dir);
            }
        });

        connect(dirTree, &QTreeView::clicked, this, [this](const QModelIndex& index) {
            QString path = dirModel->filePath(index);
            if (!path.isEmpty()) {
                rootEdit->setText(path);
                statusBar()->showMessage("Selected project root: " + path);
            }
        });

        connect(useSelectedDirButton, &QPushButton::clicked, this, [this]() {
            QModelIndex index = dirTree->currentIndex();
            QString path = dirModel->filePath(index);
            if (!path.isEmpty()) {
                rootEdit->setText(path);
                statusBar()->showMessage("Using selected project root: " + path);
            }
        });

        connect(generateButton, &QPushButton::clicked, this, [this]() {
            generateChanges();
        });

        connect(applyButton, &QPushButton::clicked, this, [this]() {
            applyDisplayedChanges();
        });

        connect(rollbackButton, &QPushButton::clicked, this, [this]() {
            rollbackLast();
        });
    }

    ~MainWindow() override {
        saveSettings();
    }

private:
    QLineEdit* rootEdit = nullptr;
    QLineEdit* backupEdit = nullptr;
    QLineEdit* modelEdit = nullptr;
    QSpinBox* maxProjectSpin = nullptr;
    QSpinBox* maxFileSpin = nullptr;
    QCheckBox* allowDeleteBox = nullptr;
    QCheckBox* allowRenameBox = nullptr;
    QCheckBox* noSyntaxBox = nullptr;
    QPushButton* generateButton = nullptr;
    QPushButton* applyButton = nullptr;
    QPushButton* rollbackButton = nullptr;
    QProgressBar* progress = nullptr;
    QSplitter* mainSplitter = nullptr;
    QSplitter* rightSplitter = nullptr;
    QFileSystemModel* dirModel = nullptr;
    QTreeView* dirTree = nullptr;
    QListWidget* fileStatusList = nullptr;
    QPlainTextEdit* changeView = nullptr;
    QTabWidget* bottomTabs = nullptr;
    QPlainTextEdit* requestEdit = nullptr;
    QPlainTextEdit* fullPromptView = nullptr;
    QPlainTextEdit* responseView = nullptr;
    QPlainTextEdit* logView = nullptr;

    std::map<QString, QListWidgetItem*> fileItems;
    std::mutex planMutex;
    ChangePlan displayedPlan;
    bool hasDisplayedPlan = false;
    std::atomic_bool running{false};

    void installMonoFonts() {
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        changeView->setFont(mono);
        fullPromptView->setFont(mono);
        responseView->setFont(mono);
        logView->setFont(mono);
    }

    void restoreSettings() {
        QSettings s("LynxGeekNYC", "ai-edit-gui-direct");
        rootEdit->setText(s.value("root", QDir::homePath()).toString());
        backupEdit->setText(s.value("backup", "/backup").toString());
        modelEdit->setText(s.value("model", "gpt-4.1").toString());
        requestEdit->setPlainText(s.value("lastRequest").toString());
    }

    void saveSettings() {
        QSettings s("LynxGeekNYC", "ai-edit-gui-direct");
        s.setValue("root", rootEdit->text());
        s.setValue("backup", backupEdit->text());
        s.setValue("model", modelEdit->text());
        s.setValue("lastRequest", requestEdit->toPlainText());
    }

    void syncDirectoryTreeToRootEdit() {
        QString root = rootEdit->text();
        if (root.isEmpty()) {
            root = QDir::homePath();
        }
        QModelIndex idx = dirModel->index(root);
        if (idx.isValid()) {
            dirTree->setCurrentIndex(idx);
            dirTree->scrollTo(idx);
            dirTree->expand(idx);
        }
    }

    AppConfig collectConfig(bool requireRequest = true) {
        AppConfig c;
        c.root = rootEdit->text().toStdString();
        c.backupRoot = backupEdit->text().toStdString();
        c.model = modelEdit->text().trimmed().toStdString();
        c.request = requestEdit->toPlainText().trimmed().toStdString();
        c.maxProjectBytes = static_cast<size_t>(maxProjectSpin->value());
        c.maxFileBytes = static_cast<size_t>(maxFileSpin->value());
        c.allowDelete = allowDeleteBox->isChecked();
        c.allowRename = allowRenameBox->isChecked();
        c.noSyntaxCheck = noSyntaxBox->isChecked();

        if (c.root.empty()) {
            throw std::runtime_error("Project root is required.");
        }
        if (!fs::exists(c.root) || !fs::is_directory(c.root)) {
            throw std::runtime_error("Invalid project root.");
        }
        c.root = fs::canonical(c.root);

        if (c.backupRoot.empty()) {
            throw std::runtime_error("Backup root is required.");
        }
        if (c.model.empty()) {
            throw std::runtime_error("Model is required.");
        }
        if (requireRequest && c.request.empty()) {
            throw std::runtime_error("User prompt is required.");
        }
        return c;
    }

    UiHooks makeUiHooks() {
        UiHooks ui;
        ui.log = [this](const std::string& s) { appendLog(QString::fromStdString(s)); };
        ui.changes = [this](const std::string& s) { setChangePreview(QString::fromStdString(s)); };
        ui.prompt = [this](const std::string& s) { setFullPrompt(QString::fromStdString(s)); };
        ui.response = [this](const std::string& s) { setResponse(QString::fromStdString(s)); };
        ui.fileStatus = [this](const std::string& file, const std::string& status) {
            setFileStatus(QString::fromStdString(file), QString::fromStdString(status));
        };
        return ui;
    }

    void setRunning(bool value) {
        QMetaObject::invokeMethod(this, [this, value]() {
            running = value;
            generateButton->setEnabled(!value);
            applyButton->setEnabled(!value && hasDisplayedPlan);
            rollbackButton->setEnabled(!value);
            progress->setVisible(value);
            statusBar()->showMessage(value ? "Working..." : "Ready");
        }, Qt::QueuedConnection);
    }

    void appendLog(const QString& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            logView->appendPlainText("[" + stamp + "] " + s);
            logView->moveCursor(QTextCursor::End);
            statusBar()->showMessage(s.left(180));
        }, Qt::QueuedConnection);
    }

    void setChangePreview(const QString& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            changeView->setPlainText(s);
            changeView->moveCursor(QTextCursor::Start);
        }, Qt::QueuedConnection);
    }

    void setFullPrompt(const QString& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            fullPromptView->setPlainText(s);
            bottomTabs->setCurrentWidget(fullPromptView);
        }, Qt::QueuedConnection);
    }

    void setResponse(const QString& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            responseView->setPlainText(s);
            bottomTabs->setCurrentWidget(responseView);
        }, Qt::QueuedConnection);
    }

    void setFileStatus(const QString& file, const QString& status) {
        QMetaObject::invokeMethod(this, [this, file, status]() {
            QListWidgetItem* item = nullptr;
            auto it = fileItems.find(file);
            if (it == fileItems.end()) {
                item = new QListWidgetItem(fileStatusList);
                fileItems[file] = item;
            } else {
                item = it->second;
            }
            item->setText(file + "    [" + status + "]");
            fileStatusList->scrollToItem(item);
        }, Qt::QueuedConnection);
    }

    void clearRunViews() {
        fileItems.clear();
        fileStatusList->clear();
        changeView->clear();
        fullPromptView->clear();
        responseView->clear();
        logView->clear();
        {
            std::lock_guard<std::mutex> lock(planMutex);
            displayedPlan = ChangePlan{};
            hasDisplayedPlan = false;
        }
        applyButton->setEnabled(false);
    }

    void showError(const QString& msg) {
        QMetaObject::invokeMethod(this, [this, msg]() {
            QMessageBox::critical(this, "AI Edit Error", msg);
            statusBar()->showMessage("Error: " + msg.left(160));
        }, Qt::QueuedConnection);
    }

    bool askYesNoBlocking(const QString& title, const QString& text) {
        if (QThread::currentThread() == this->thread()) {
            return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes;
        }

        bool result = false;
        QMetaObject::invokeMethod(this, [&]() {
            result = QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes;
        }, Qt::BlockingQueuedConnection);
        return result;
    }

    void generateChanges() {
        if (running) {
            return;
        }

        AppConfig c;
        try {
            c = collectConfig();
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Invalid Settings", e.what());
            return;
        }

        saveSettings();
        syncDirectoryTreeToRootEdit();
        clearRunViews();
        setRunning(true);

        std::thread([this, c]() {
            UiHooks ui = makeUiHooks();
            try {
                fs::create_directories(aiDir(c));

                auto files = listProjectFiles(c, ui);
                auto context = buildProjectContext(c, files, ui);
                auto outputText = callOpenAI(c, context, ui);
                auto plan = parseChangePlan(outputText);

                validateChangePlan(c, plan);
                saveChangePlan(c, plan);

                std::string preview = renderChangePreview(c, plan);
                ui.changes(preview);

                {
                    std::lock_guard<std::mutex> lock(planMutex);
                    displayedPlan = plan;
                    hasDisplayedPlan = true;
                }

                for (const auto& rel : touchedFiles(plan)) {
                    ui.fileStatus(rel, "change ready");
                }

                ui.log("Change plan saved to " + (aiDir(c) / "last-change-plan.json").string());
                ui.log("Review the main code-change pane, then click Apply Displayed Changes.");
            } catch (const std::exception& e) {
                ui.log(std::string("ERROR: ") + e.what());
                showError(QString::fromStdString(e.what()));
            }
            setRunning(false);
        }).detach();
    }

    void applyDisplayedChanges() {
        if (running) {
            return;
        }

        AppConfig c;
        ChangePlan plan;
        try {
            c = collectConfig();
            std::lock_guard<std::mutex> lock(planMutex);
            plan = displayedPlan;
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Invalid Settings", e.what());
            return;
        }

        if (plan.changes.empty()) {
            QMessageBox::information(this, "No Changes", "There are no displayed changes to apply.");
            return;
        }

        bool hasDelete = false;
        bool hasRename = false;
        for (const auto& ch : plan.changes) {
            if (ch.action == "delete") {
                hasDelete = true;
            }
            if (ch.action == "rename") {
                hasRename = true;
            }
        }

        if (hasDelete && !c.allowDelete) {
            QMessageBox::warning(this, "Deletion Blocked", "The change plan deletes files. Enable Allow delete first.");
            return;
        }
        if (hasRename && !c.allowRename) {
            QMessageBox::warning(this, "Rename Blocked", "The change plan renames files. Enable Allow rename first.");
            return;
        }
        if (hasDelete) {
            if (!askYesNoBlocking("Confirm Delete", "This change plan deletes one or more files. Apply anyway?")) {
                return;
            }
        }
        if (hasRename) {
            if (!askYesNoBlocking("Confirm Rename", "This change plan renames one or more files. Apply anyway?")) {
                return;
            }
        }
        if (!askYesNoBlocking("Apply Changes", "Apply the displayed changes to the project now? Backups will be created first.")) {
            return;
        }

        setRunning(true);
        std::thread([this, c, plan]() {
            UiHooks ui = makeUiHooks();
            try {
                applyChangePlanCore(c, plan, ui);
            } catch (const std::exception& e) {
                ui.log(std::string("ERROR: ") + e.what());
                showError(QString::fromStdString(e.what()));
            }
            setRunning(false);
        }).detach();
    }

    void rollbackLast() {
        if (running) {
            return;
        }

        AppConfig c;
        try {
            c = collectConfig(false);
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Invalid Settings", e.what());
            return;
        }

        if (!askYesNoBlocking("Rollback Last", "Rollback the last applied AI edit for this project?")) {
            return;
        }

        setRunning(true);
        std::thread([this, c]() {
            UiHooks ui = makeUiHooks();
            try {
                rollbackLastCore(c, ui);
            } catch (const std::exception& e) {
                ui.log(std::string("ERROR: ") + e.what());
                showError(QString::fromStdString(e.what()));
            }
            setRunning(false);
        }).detach();
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AI Edit GUI Direct");
    QApplication::setOrganizationName("LynxGeekNYC");

    curl_global_init(CURL_GLOBAL_DEFAULT);

    MainWindow w;
    w.show();

    int rc = app.exec();
    curl_global_cleanup();
    return rc;
}
