# AI Edit TUI

`ai-edit-tui.cpp` is a terminal interface version of the OpenAI SSH Project Editor. It is intended for SSH sessions where a Qt desktop GUI is not practical.

## Files

- `ai-edit-tui.cpp`, terminal UI C++20 application using ncurses, libcurl, and nlohmann/json.
- `install-ai-edit-tui.sh`, dependency installer for AlmaLinux/Rocky/RHEL/CentOS/Fedora, Debian/Ubuntu/Mint/Kali, Arch/Manjaro, openSUSE/SUSE, and Alpine.

## Install dependencies

```bash
chmod +x install-ai-edit-tui.sh
./install-ai-edit-tui.sh
```

The script asks which distro family you use, installs the matching packages, then offers to compile the app and optionally install it to `/usr/local/bin/ai-edit-tui`.

## Manual build

```bash
g++ -std=c++20 -O2 -Wall -Wextra ai-edit-tui.cpp -o ai-edit-tui -lcurl -lncurses
```

## Run

```bash
export OPENAI_API_KEY="your_api_key_here"
./ai-edit-tui /path/to/project
```

Optional flags:

```bash
./ai-edit-tui /path/to/project --model gpt-4.1 --backup-root /backup
./ai-edit-tui /path/to/project --no-syntax-check
```

You can also set the model with:

```bash
export AI_EDIT_MODEL="gpt-4.1"
```

## Keys

- `Tab`, switch focus between files, preview, and log.
- `Enter`, view the selected file in the preview pane.
- `Ctrl+P`, edit the AI request in `$EDITOR`, default is nano.
- `Ctrl+G`, generate the OpenAI change plan.
- `Ctrl+A`, apply the generated change plan.
- `Ctrl+R`, roll back the last applied edit.
- `Ctrl+O`, open the selected file in `$EDITOR`.
- `Ctrl+F`, rescan files.
- `d`, toggle whether delete actions are allowed.
- `n`, toggle whether rename actions are allowed.
- `s`, toggle syntax checks.
- `q` or `Esc`, quit when no worker is active.

## Safety behavior

- It rejects absolute paths, parent directory traversal, and `.env` edits.
- It skips generated and dependency folders like `.git`, `node_modules`, `vendor`, `build`, `dist`, cache folders, and upload folders.
- It backs up every touched file before applying changes.
- It stores internal data in `.ai-edit/` under the project root.
- It performs syntax checks for PHP, Python, JavaScript, and JSON unless disabled.
