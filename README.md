# OpenAI SSH Project Editor
AI SSH Project Editor is a local server-side C++ utility that allows you to make controlled code changes to a project directory using the OpenAI API. I got tired of using VS Code, uploading, downloading, etc.
This automates the process for when I need ChatGPT to make a change to my projects. Directly within the server using SSH.

The tool is designed to be run manually after logging into your server through SSH. It does not give the AI unrestricted shell access. Instead, it scans a specific project directory, sends relevant project files to OpenAI, receives a Git-compatible patch, shows the patch to the user, backs up only the changed files, and applies the patch only after confirmation.

## Main Features

- Runs locally from SSH
- Restricts edits to one project root directory
- Uses the OpenAI API to generate Git-compatible unified diffs
- Shows the patch before applying changes
- Requires typed confirmation before applying
- Automatically backs up only the files being changed
- Preserves project-relative backup paths
- Supports rollback of the last applied change
- Requires extra confirmation for file deletion
- Requires extra confirmation for file rename
- Blocks `.env` edits
- Skips large/unwanted folders such as `.git`, `node_modules`, `vendor`, `uploads`, `cache`, and backups
- Runs PHP syntax checks after changes
- Keeps logs and patch history in `.ai-edit`

## Backup Behavior

Before any patch is applied, the tool automatically backs up only the files that will be changed.

Backups are stored in:

```bash
/backup/<timestamp>/<relative-file-path>
