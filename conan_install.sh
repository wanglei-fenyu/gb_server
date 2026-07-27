#!/usr/bin/env bash
set -euo pipefail

# Quick Conan installer for this repo.
# Default is tuned for Linux clang workflow in this project.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PROFILE_HOST="profiles/clang_debug_pr"
PROFILE_BUILD="profiles/clang_debug_pr"
CPPSTD="23"
BUILD_MISSING=1
DO_CLEAN=0

print_help() {
  cat <<'EOF'
Usage:
  ./conan_install.sh [options] [-- <extra conan install args>]

Options:
  --clean                    Clear local Conan cache before install
  --profile <path>           Host profile (default: profiles/clang_debug_pr)
  --build-profile <path>     Build profile (default: same as host)
  --cppstd <value>           Host compiler.cppstd (default: gnu17)
  --no-build-missing         Do not use --build=missing
  -h, --help                 Show this help

Examples:
  ./conan_install.sh
  ./conan_install.sh --clean
  ./conan_install.sh --profile profiles/clang_release_pr --cppstd gnu20
  ./conan_install.sh -- --update
EOF
}

EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      DO_CLEAN=1
      shift
      ;;
    --profile)
      PROFILE_HOST="$2"
      PROFILE_BUILD="$2"
      shift 2
      ;;
    --build-profile)
      PROFILE_BUILD="$2"
      shift 2
      ;;
    --cppstd)
      CPPSTD="$2"
      shift 2
      ;;
    --no-build-missing)
      BUILD_MISSING=0
      shift
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        EXTRA_ARGS+=("$1")
        shift
      done
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

if ! command -v conan >/dev/null 2>&1; then
  echo "[ERROR] conan not found in PATH"
  exit 1
fi

echo "== Conan quick install =="
echo "Host profile : $PROFILE_HOST"
echo "Build profile: $PROFILE_BUILD"
echo "cppstd       : $CPPSTD"

if [[ $DO_CLEAN -eq 1 ]]; then
  echo "[1/2] Cleaning local Conan cache..."
  conan remove "*" -c
  echo "[OK] Cache cleaned"
fi

echo "[2/2] Installing dependencies..."
CMD=(conan install . -pr:h="$PROFILE_HOST" -pr:b="$PROFILE_BUILD" -s:h "compiler.cppstd=$CPPSTD")
if [[ $BUILD_MISSING -eq 1 ]]; then
  CMD+=(--build=missing)
fi
CMD+=("${EXTRA_ARGS[@]}")

printf 'Running:'
printf ' %q' "${CMD[@]}"
printf '\n'
"${CMD[@]}"

echo "[SUCCESS] Conan install finished"
