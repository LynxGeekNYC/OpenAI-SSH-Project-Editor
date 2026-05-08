# OpenAI SSH Project Editor, Native Linux GUI

A native Linux desktop application that uses the OpenAI API to inspect, edit, create, rename, and delete files inside a selected project directory. The app is designed for developers, server administrators, and power users who want an AI-assisted project editor that works directly on local Linux files without requiring Git.

The application provides a three-pane interface:

1. **Left pane**, a mouse-selectable directory browser and live file-status list.
2. **Main upper pane**, a live preview of file changes before they are applied.
3. **Bottom pane**, prompt input, full prompt sent to OpenAI, raw OpenAI response, and run log.

This project does **not** require Git at runtime. It applies changes directly to the filesystem after creating backups.

---

## Important Warning

This tool can modify files in the selected directory. Always review the generated change plan before applying it.

The app creates backups before writing, deleting, or renaming files, but you should still avoid running it against critical production directories unless you understand exactly what the generated changes will do.

Recommended use:

- Test on a copy of your project first.
- Use a non-root user when possible.
- Keep your backup folder outside the project directory.
- Review every generated change before clicking **Apply Displayed Changes**.
- Never store secrets directly inside project files.

---

## Features

### Native Linux GUI

- Built with Qt6 Widgets.
- Runs as a desktop application on Linux.
- Uses a resizable three-pane layout.
- Keeps the interface responsive while OpenAI processing happens in a background thread.

### Mouse-Selectable Working Directory

- The left pane contains a clickable directory tree.
- Click any folder to select it as the project root.
- A **Use Selected Folder** button is also included.
- A traditional **Browse** button is available at the top.

### OpenAI-Powered File Editing

The app sends selected project context to OpenAI and asks for a structured JSON change plan. The returned plan can include:

- `write`, replace an existing file with complete new content.
- `create`, create a new file with complete content.
- `delete`, delete a file, only when explicitly allowed.
- `rename`, rename a file, only when explicitly allowed.

### No Git Dependency

This version does not use:

- `git apply`
- Git patches
- Git repositories
- Git commits
- Git status
- Git branches

It works directly with files using C++ filesystem operations.

### Live File Status

The side pane shows live status messages such as:

- scanned
- included in OpenAI context
- change ready
- backed up
- written
- created
- deleted
- renamed
- syntax OK
- restored

### Change Preview

The main pane shows a readable preview of what will change before anything is applied.

For each file, the preview shows:

- action type
- file path
- new path for rename operations
- old lines and new lines where practical
- deletion preview for files that will be removed

### Backup and Rollback

Before changes are applied, the app backs up touched files to:

```text
/backup/<timestamp>/
```

It also stores internal rollback data in:

```text
<project-root>/.ai-edit/backups/<timestamp>/
```

The **Rollback Last** button restores the most recent applied AI edit for the selected project.

### Syntax Checks

After changes are applied, the app can run syntax checks for supported file types:

| File Type | Check Used |
|---|---|
| `.php` | `php -l` |
| `.py` | `python3 -m py_compile` |
| `.js`, `.mjs`, `.cjs` | `node --check` |
| `.json` | JSON parser validation |

You can disable these checks with the **Skip syntax checks** checkbox.

### Safety Restrictions

The app blocks unsafe file paths and sensitive file targets.

Blocked by default:

- absolute paths
- paths containing `..`
- `.env` files
- files outside the selected project root
- delete operations unless **Allow delete** is checked
- rename operations unless **Allow rename** is checked

---

## Requirements

### Operating System

Tested target platform:

- Linux desktop or Linux server with X11 or Wayland GUI support

Recommended distributions:

- Ubuntu
- Debian
- Linux Mint
- Fedora
- Arch Linux

### Compiler and Libraries

Required:

- C++20 compiler, such as `g++`
- Qt6 Widgets
- libcurl
- nlohmann-json
- pkg-config

Optional but recommended:

- PHP CLI, for PHP syntax checks
- Python 3, for Python syntax checks
- Node.js, for JavaScript syntax checks

### OpenAI API Key

You need an OpenAI API key. The application reads it from the environment variable:

```bash
OPENAI_API_KEY
```

---

## Installation

### Ubuntu or Debian

Install dependencies:

```bash
sudo apt update
sudo apt install build-essential pkg-config qt6-base-dev libcurl4-openssl-dev nlohmann-json3-dev php-cli python3 nodejs
```

Build the app:

```bash
g++ -std=c++20 ai-edit-gui.cpp -o ai-edit-gui $(pkg-config --cflags --libs Qt6Widgets) -lcurl
```

Run the app:

```bash
export OPENAI_API_KEY="your_openai_api_key_here"
./ai-edit-gui
```

---

### Fedora

Install dependencies:

```bash
sudo dnf install gcc-c++ pkgconf-pkg-config qt6-qtbase-devel libcurl-devel nlohmann-json-devel php-cli python3 nodejs
```

Build:

```bash
g++ -std=c++20 ai-edit-gui.cpp -o ai-edit-gui $(pkg-config --cflags --libs Qt6Widgets) -lcurl
```

Run:

```bash
export OPENAI_API_KEY="your_openai_api_key_here"
./ai-edit-gui
```

---

### Arch Linux

Install dependencies:

```bash
sudo pacman -S base-devel pkgconf qt6-base curl nlohmann-json php python nodejs
```

Build:

```bash
g++ -std=c++20 ai-edit-gui.cpp -o ai-edit-gui $(pkg-config --cflags --libs Qt6Widgets) -lcurl
```

Run:

```bash
export OPENAI_API_KEY="your_openai_api_key_here"
./ai-edit-gui
```

---

## Optional System-Wide Install

After building the binary, you can install it system-wide:

```bash
sudo install -m 755 ai-edit-gui /usr/local/bin/ai-edit-gui
```

Then run it from anywhere:

```bash
export OPENAI_API_KEY="your_openai_api_key_here"
ai-edit-gui
```

---

## Optional Desktop Launcher

Create a desktop launcher:

```bash
mkdir -p ~/.local/share/applications
nano ~/.local/share/applications/ai-edit-gui.desktop
```

Paste this content:

```ini
[Desktop Entry]
Type=Application
Name=AI Edit GUI
Comment=OpenAI-powered Linux project editor
Exec=/usr/local/bin/ai-edit-gui
Terminal=false
Categories=Development;Utility;
```

Then update the desktop database if your system supports it:

```bash
update-desktop-database ~/.local/share/applications 2>/dev/null || true
```

Important: if you launch from a desktop icon, your shell environment may not include `OPENAI_API_KEY`. In that case, start the app from a terminal, or configure your desktop environment to provide that variable.

---

## Basic Usage

### 1. Start the Application

```bash
export OPENAI_API_KEY="your_openai_api_key_here"
./ai-edit-gui
```

### 2. Select a Project Directory

You have three options:

- Click a folder in the left directory tree.
- Click **Use Selected Folder**.
- Use the top **Browse** button.

The selected folder becomes the project root. The app will only read and write files inside that directory.

### 3. Choose a Backup Directory

The default backup directory is:

```text
/backup
```

You can change it in the **Backup root** field.

Make sure your user account has permission to write to the selected backup directory.

If `/backup` does not exist or is not writable, create it:

```bash
sudo mkdir -p /backup
sudo chown "$USER:$USER" /backup
```

### 4. Enter a Prompt

In the bottom **User Prompt** tab, describe what you want the app to change.

Example prompts:

```text
Add better error handling to process_files.php and make the JSON responses consistent.
```

```text
Create a Bootstrap admin dashboard page that lists users, projects, statuses, and recent notes.
```

```text
Update all PHP files to use prepared statements instead of direct SQL string concatenation.
```

```text
Review the JavaScript files and fix any broken event handlers related to the upload form.
```

### 5. Generate Changes

Click:

```text
Generate Changes
```

The app will:

1. Scan the selected project directory.
2. Skip large or unsupported files.
3. Build project context.
4. Send the prompt and context to OpenAI.
5. Parse the returned JSON change plan.
6. Display the planned file changes in the main pane.

### 6. Review the Main Change Pane

Before applying, review:

- which files will be changed
- which files will be created
- whether any file will be deleted
- whether any file will be renamed
- whether the generated content makes sense

### 7. Apply the Changes

Click:

```text
Apply Displayed Changes
```

The app will:

1. Validate paths.
2. Create backups.
3. Write, create, delete, or rename files.
4. Run syntax checks unless disabled.
5. Save internal rollback data.
6. Write a run log.

### 8. Roll Back if Needed

Click:

```text
Rollback Last
```

This restores the last applied AI edit for the selected project.

---

## OpenAI Response Format

The app expects OpenAI to return strict JSON in this format:

```json
{
  "summary": "Short summary of the planned changes.",
  "changes": [
    {
      "action": "write",
      "path": "relative/file/path.php",
      "content": "Complete final file content here."
    }
  ]
}
```

Supported actions:

### Write Existing File

```json
{
  "action": "write",
  "path": "index.php",
  "content": "<?php echo 'Updated file'; ?>"
}
```

### Create New File

```json
{
  "action": "create",
  "path": "admin/dashboard.php",
  "content": "<?php echo 'Dashboard'; ?>"
}
```

### Delete File

Delete requires the **Allow delete** checkbox.

```json
{
  "action": "delete",
  "path": "old-file.php"
}
```

### Rename File

Rename requires the **Allow rename** checkbox.

```json
{
  "action": "rename",
  "path": "old-name.php",
  "new_path": "new-name.php"
}
```

Rename with new content:

```json
{
  "action": "rename",
  "path": "old-name.php",
  "new_path": "new-name.php",
  "content": "<?php echo 'Renamed and updated'; ?>"
}
```

---

## Files and Folders Created by the App

Inside the selected project root, the app creates:

```text
.ai-edit/
```

This folder stores:

```text
.ai-edit/last-change-plan.json
.ai-edit/last-session.txt
.ai-edit/ai-edit.log
.ai-edit/backups/<timestamp>/
```

External backups are stored in:

```text
/backup/<timestamp>/
```

or whatever backup root you selected.

---

## Supported File Types

The scanner includes common source and project files:

```text
.php
.inc
.html
.htm
.css
.js
.jsx
.ts
.tsx
.json
.sql
.txt
.md
.xml
.yml
.yaml
.conf
.ini
.cpp
.c
.h
.hpp
.py
.sh
.rb
.go
.rs
.java
.cs
.env.example
.dockerfile
.htaccess
Dockerfile
```

The scanner intentionally excludes `.env` files.

---

## Skipped Directories

The app skips common dependency, cache, build, and upload directories:

```text
.ai-edit
node_modules
vendor
cache
tmp
logs
uploads
backup
backups
.idea
.vscode
dist
build
target
__pycache__
```

This keeps requests smaller and avoids sending unnecessary or sensitive generated files to OpenAI.

---

## Configuration Options

### Project Root

The directory the app will scan and edit.

### Backup Root

Where external backups are stored. Default:

```text
/backup
```

### Model

Default:

```text
gpt-4.1
```

You may change this to another OpenAI model available to your API account.

### Max Project Bytes

Controls how much project content is sent to OpenAI.

Default:

```text
220000 bytes
```

Increase this for larger projects, but remember that larger prompts use more tokens.

### Max File Bytes

Controls the maximum file size included in the OpenAI context.

Default:

```text
60000 bytes
```

### Allow Delete

When unchecked, delete operations are blocked.

### Allow Rename

When unchecked, rename operations are blocked.

### Skip Syntax Checks

When checked, the app will not run syntax checks after applying changes.

---

## Security Notes

This app sends project file content to the OpenAI API. Do not include secrets, private keys, passwords, credentials, medical records, client records, or other sensitive material unless your use case, account configuration, and compliance requirements allow it.

The app attempts to avoid sensitive files by blocking `.env`, but it cannot automatically know every file that may contain confidential data.

Before using this tool in a professional environment, review:

- your data handling requirements
- your client confidentiality obligations
- your regulatory requirements
- your OpenAI account and API data settings
- your project contents

Recommended protections:

- keep secrets in `.env`, which this app blocks
- avoid hardcoding credentials in source files
- run against a copy of the project first
- restrict filesystem permissions
- review the generated OpenAI prompt tab before applying changes

---

## Troubleshooting

### Missing OpenAI API Key

Error:

```text
Missing OPENAI_API_KEY environment variable.
```

Fix:

```bash
export OPENAI_API_KEY="your_openai_api_key_here"
./ai-edit-gui
```

To make it persistent for terminal sessions, add it to your shell profile:

```bash
echo 'export OPENAI_API_KEY="your_openai_api_key_here"' >> ~/.bashrc
source ~/.bashrc
```

Use the appropriate profile file for your shell if you do not use Bash.

---

### Qt6 Not Found During Build

Error example:

```text
Package Qt6Widgets was not found in the pkg-config search path.
```

Fix on Ubuntu or Debian:

```bash
sudo apt install qt6-base-dev pkg-config
```

Then confirm Qt6 is visible:

```bash
pkg-config --cflags --libs Qt6Widgets
```

---

### Permission Denied Writing Backups

Error example:

```text
Cannot write file: /backup/<timestamp>/...
```

Fix:

```bash
sudo mkdir -p /backup
sudo chown "$USER:$USER" /backup
```

Or choose a backup directory inside your home folder, for example:

```text
/home/youruser/ai-edit-backups
```

---

### Syntax Check Failed

If a syntax check fails, the app reports the file and checker output in the run log.

You can:

1. Review the generated file.
2. Fix the issue manually.
3. Use **Rollback Last**.
4. Regenerate with a more specific prompt.

---

### OpenAI Returned Invalid JSON

The app expects strict JSON. If OpenAI returns commentary, markdown, or malformed JSON, the parse will fail.

Try a more direct prompt:

```text
Modify only index.php. Return a valid change plan. Do not include explanation.
```

The app also instructs the model to return JSON only, but model output can still occasionally be invalid.

---

### App Opens but Directory Tree is Empty

Possible causes:

- filesystem permissions
- running inside a restricted sandbox
- no GUI filesystem access
- invalid home directory

Try launching from a terminal and selecting a folder with the top **Browse** button.

---

## Recommended Workflow

For safest use:

1. Copy the project to a test directory.
2. Launch the app.
3. Select the test directory in the side pane.
4. Enter a specific prompt.
5. Generate changes.
6. Review the change preview.
7. Apply changes.
8. Test the app manually.
9. Move the changes into your real project only after verification.

---

## Example Use Cases

### PHP Project Cleanup

```text
Review the PHP files and replace direct SQL queries with prepared statements where possible. Keep the current database connection style.
```

### Bootstrap UI Enhancement

```text
Improve the dashboard page using Bootstrap 5 cards, tables, and mobile-friendly spacing. Do not change backend logic.
```

### JavaScript Bug Fix

```text
Find and fix the upload form JavaScript so the progress bar updates correctly and errors appear in the alert box.
```

### Add Logging

```text
Add basic application logging to the PHP processing scripts. Do not log passwords, API keys, tokens, or uploaded file contents.
```

### Create a New Admin Page

```text
Create admin/users.php with a Bootstrap table showing users, emails, role, status, and created date. Use the existing header and footer files if present.
```

---

## Project Layout Suggestion

A simple repository layout:

```text
OpenAI-SSH-Project-Editor/
├── ai-edit-gui.cpp
├── README.md
├── LICENSE
└── screenshots/
    ├── main-window.png
    ├── directory-browser.png
    └── change-preview.png
```

---

## Development Notes

The current implementation is a single C++ source file for easier building and deployment.

Major components:

| Component | Purpose |
|---|---|
| `MainWindow` | Qt6 GUI and event handling |
| `QFileSystemModel` | Directory browser in the left pane |
| `QTreeView` | Mouse-selectable folder tree |
| `QPlainTextEdit` | Prompt, response, logs, and change preview |
| `callOpenAI()` | Sends project context to the OpenAI API |
| `parseChangePlan()` | Parses the JSON returned by OpenAI |
| `applyChangePlanCore()` | Applies file operations directly |
| `autoBackupTouchedFiles()` | Creates backup copies before edits |
| `rollbackLastCore()` | Restores the last applied edit |
| `runSyntaxChecks()` | Runs language-specific syntax checks |

---

## Limitations

- The app does not do semantic diff merging.
- For `write` operations, OpenAI must provide complete final file content.
- Very large projects may need higher context limits or narrower prompts.
- Binary files are not supported.
- `.env` files are intentionally blocked.
- Rollback only tracks the last applied AI edit per selected project.
- Syntax checking only covers selected file types.

---

## Future Improvements

Potential enhancements:

- Add a dedicated file include and exclude editor.
- Add per-file checkboxes before applying changes.
- Add syntax highlighting in the change preview pane.
- Add token usage and estimated cost display.
- Add streaming OpenAI responses.
- Add support for multiple rollback sessions.
- Add a project profile system.
- Add a settings screen for ignored folders and file extensions.
- Add a local-only dry-run mode.
- Add a built-in file viewer for selected files.

---

## License

Add your preferred license in a `LICENSE` file.

Suggested options:

- MIT License for a permissive open-source project.
- GPLv3 if you want derivative works to remain open source.
- Proprietary license if you plan to distribute it commercially.

---

## Author

Created by LynxGeekNYC.

Repository name suggestion:

```text
OpenAI-SSH-Project-Editor
```

---

## Disclaimer

This application is an AI-assisted file editor. Generated changes may be incomplete, incorrect, insecure, or incompatible with your project. Review all generated changes before applying them. You are responsible for testing, securing, and validating any code generated or modified by this tool.
