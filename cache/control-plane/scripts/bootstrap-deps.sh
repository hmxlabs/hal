#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")"/.. && pwd)"

info() {
  echo " [*] $*"
}

warn() {
  echo " [!] $*"
}

run_as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    if command -v sudo >/dev/null 2>&1; then
      sudo "$@"
    else
      echo "error: command requires root privileges, but sudo is not available" >&2
      exit 1
    fi
  fi
}

cmd_exists() {
  command -v "$1" >/dev/null 2>&1
}

verify_dependency_commands() {
  local missing=()
  local command_name

  for command_name in "${DEPENDENCY_COMMANDS[@]}"; do
    if ! cmd_exists "${command_name}"; then
      missing+=("${command_name}")
    fi
  done

  if ((${#missing[@]} > 0)); then
    warn "Missing runtime commands: ${missing[*]}"
    warn "Retry after checking your package manager installation."
    exit 1
  fi

  if cmd_exists pkg-config; then
    if ! pkg-config --exists hiredis jansson libmicrohttpd; then
      warn "pkg-config is present but dependency metadata is unavailable for one or more packages."
      warn "Build will try fallback flags, but explicit pkg-config support is recommended."
    fi
  fi
}

install_brew() {
  if ! cmd_exists brew; then
    echo "error: brew not found. Install Homebrew from https://brew.sh and rerun." >&2
    exit 1
  fi

  if cmd_exists brew; then
    info "Updating Homebrew"
    brew update
  fi

  info "Installing Homebrew dependencies"
  local pkgs=(redis hiredis jansson libmicrohttpd pkgconf curl)
  for package in "${pkgs[@]}"; do
    if brew list --versions "${package}" >/dev/null 2>&1; then
      info "Already installed: ${package}"
    else
      brew install "${package}"
    fi
  done

  if brew list --versions hurl >/dev/null 2>&1; then
    info "Already installed: hurl"
  else
    warn "Optional package 'hurl' not installed; contract tests requiring Hurl may not run."
    if brew info hurl >/dev/null 2>&1; then
      brew install hurl
    else
      warn "Homebrew package 'hurl' unavailable on this platform."
    fi
  fi
}

install_apt() {
  local required_pkgs=(
    redis-server redis-tools
    libhiredis-dev libjansson-dev libmicrohttpd-dev pkg-config
  )
  run_as_root apt-get update
  run_as_root apt-get install -y "${required_pkgs[@]}"
  if ! run_as_root apt-get install -y hurl; then
    warn "Optional package 'hurl' unavailable in apt repositories."
  fi
}

install_dnf() {
  local required_pkgs=(
    redis redis-cli
    hiredis-devel jansson-devel libmicrohttpd-devel pkgconf-pkg-config
  )
  run_as_root dnf install -y "${required_pkgs[@]}"
  if ! run_as_root dnf install -y hurl; then
    warn "Optional package 'hurl' unavailable in dnf repositories."
  fi
}

install_yum() {
  local required_pkgs=(
    redis redis
    hiredis-devel jansson-devel libmicrohttpd-devel pkgconfig
  )
  run_as_root yum install -y "${required_pkgs[@]}"
  if ! run_as_root yum install -y hurl; then
    warn "Optional package 'hurl' unavailable in yum repositories."
  fi
}

if cmd_exists apt-get; then
  info "Detected apt package manager"
  install_apt
elif cmd_exists dnf; then
  info "Detected dnf package manager"
  install_dnf
elif cmd_exists yum; then
  info "Detected yum package manager"
  install_yum
elif cmd_exists brew; then
  info "Detected Homebrew"
  install_brew
else
  echo "error: no supported package manager detected (brew, apt, dnf, yum)." >&2
  exit 1
fi

if cmd_exists apt-get; then
  DEPENDENCY_COMMANDS=(redis-server redis-cli curl pkg-config)
elif cmd_exists pkg-config; then
  DEPENDENCY_COMMANDS=(redis-server redis-cli curl pkg-config)
elif cmd_exists pkgconf; then
  DEPENDENCY_COMMANDS=(redis-server redis-cli curl pkgconf)
else
  warn "No pkg-config compatible tool found. Build may still succeed due to fallback flags, but this is not recommended."
  DEPENDENCY_COMMANDS=(redis-server redis-cli curl)
fi
verify_dependency_commands

info "Dependency bootstrap complete."
echo "Please ensure the following are also on PATH if not covered by package installs:"
echo "  - redis-server redis-cli"
echo "  - hiredis / jansson / microhttpd headers and libraries"
echo "  - pkg-config (or pkgconf)"
echo "  - hurl (for contract tests)"
