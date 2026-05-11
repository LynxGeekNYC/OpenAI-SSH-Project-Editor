#!/usr/bin/env bash
set -euo pipefail

APP_CPP="ai-edit-tui.cpp"
APP_BIN="ai-edit-tui"
INSTALL_PATH="/usr/local/bin/ai-edit-tui"

say() {
  printf '\n%s\n' "$*"
}

run_as_root() {
  if [ "${EUID:-$(id -u)}" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    echo "This installer needs root privileges and sudo is not installed." >&2
    exit 1
  fi
}

detect_family() {
  local id="" like=""
  if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    id="${ID:-}"
    like="${ID_LIKE:-}"
  fi

  case "${id} ${like}" in
    *ubuntu*|*debian*|*linuxmint*|*kali*) echo "apt" ;;
    *almalinux*|*rocky*|*rhel*|*centos*|*fedora*) echo "dnf" ;;
    *arch*|*manjaro*|*endeavouros*) echo "pacman" ;;
    *opensuse*|*suse*) echo "zypper" ;;
    *alpine*) echo "apk" ;;
    *) echo "unknown" ;;
  esac
}

install_apt() {
  say "Installing dependencies for Debian, Ubuntu, Mint, or Kali..."
  run_as_root apt update
  run_as_root apt install -y \
    build-essential \
    pkg-config \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    libncurses-dev \
    php-cli \
    python3 \
    nodejs
}

install_dnf() {
  say "Installing dependencies for AlmaLinux, Rocky, RHEL, CentOS, or Fedora..."
  run_as_root dnf install -y dnf-plugins-core || true
  run_as_root dnf config-manager --set-enabled crb >/dev/null 2>&1 || true
  run_as_root dnf config-manager --set-enabled powertools >/dev/null 2>&1 || true
  run_as_root dnf install -y epel-release || true
  run_as_root dnf install -y \
    gcc-c++ \
    make \
    pkgconf-pkg-config \
    libcurl-devel \
    nlohmann-json-devel \
    ncurses-devel \
    php-cli \
    python3 \
    nodejs \
    npm
}

install_pacman() {
  say "Installing dependencies for Arch, Manjaro, or EndeavourOS..."
  run_as_root pacman -Syu --needed --noconfirm \
    base-devel \
    pkgconf \
    curl \
    nlohmann-json \
    ncurses \
    php \
    python \
    nodejs \
    npm
}

install_zypper() {
  say "Installing dependencies for openSUSE or SUSE..."
  run_as_root zypper --non-interactive refresh
  run_as_root zypper --non-interactive install \
    gcc-c++ \
    make \
    pkg-config \
    libcurl-devel \
    nlohmann_json-devel \
    ncurses-devel \
    php-cli \
    python3 \
    nodejs \
    npm
}

install_apk() {
  say "Installing dependencies for Alpine Linux..."
  run_as_root apk update
  run_as_root apk add \
    build-base \
    pkgconf \
    curl-dev \
    nlohmann-json \
    ncurses-dev \
    php-cli \
    python3 \
    nodejs \
    npm
}

compile_app() {
  if [ ! -f "$APP_CPP" ]; then
    say "Could not find $APP_CPP in the current directory. Dependency install is complete."
    return 0
  fi

  say "Compiling $APP_CPP..."
  g++ -std=c++20 -O2 -Wall -Wextra "$APP_CPP" -o "$APP_BIN" -lcurl -lncurses
  chmod +x "$APP_BIN"
  say "Compiled ./$APP_BIN"

  read -r -p "Install $APP_BIN to $INSTALL_PATH? [y/N]: " install_choice
  case "$install_choice" in
    y|Y|yes|YES)
      run_as_root install -m 0755 "$APP_BIN" "$INSTALL_PATH"
      say "Installed to $INSTALL_PATH"
      ;;
    *)
      say "Leaving binary in current directory."
      ;;
  esac
}

main() {
  say "AI Edit TUI dependency installer"
  echo "Choose your Linux distro family:"
  echo "  1) AlmaLinux, Rocky, RHEL, CentOS, Fedora"
  echo "  2) Debian, Ubuntu, Mint, Kali"
  echo "  3) Arch, Manjaro, EndeavourOS"
  echo "  4) openSUSE, SUSE"
  echo "  5) Alpine"
  echo "  6) Auto detect"
  read -r -p "Selection [6]: " choice
  choice="${choice:-6}"

  family=""
  case "$choice" in
    1) family="dnf" ;;
    2) family="apt" ;;
    3) family="pacman" ;;
    4) family="zypper" ;;
    5) family="apk" ;;
    6) family="$(detect_family)" ;;
    *) echo "Invalid selection." >&2; exit 1 ;;
  esac

  case "$family" in
    apt) install_apt ;;
    dnf) install_dnf ;;
    pacman) install_pacman ;;
    zypper) install_zypper ;;
    apk) install_apk ;;
    *)
      echo "Could not detect your distro family." >&2
      echo "Run this script again and choose the closest option manually." >&2
      exit 1
      ;;
  esac

  say "Dependencies installed."
  read -r -p "Compile ai-edit-tui now if ai-edit-tui.cpp is present? [Y/n]: " build_choice
  build_choice="${build_choice:-Y}"
  case "$build_choice" in
    y|Y|yes|YES) compile_app ;;
    *) say "Skipping compile." ;;
  esac

  say "Done."
  echo "Run with:"
  echo "  export OPENAI_API_KEY=\"your_api_key_here\""
  echo "  ./ai-edit-tui /path/to/project"
  echo "Or if installed system-wide:"
  echo "  ai-edit-tui /path/to/project"
}

main "$@"
