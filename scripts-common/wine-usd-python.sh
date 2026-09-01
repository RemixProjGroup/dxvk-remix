#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "${script_dir}/.." && pwd)
python_exe="${repo_root}/src/usd-plugins/_external/python/python.exe"

to_windows_path() {
  winepath -w "$1"
}

for variable in USD_SCHEMA_PYTHON USD_NVUSD_BINDIR USD_LIBPYTHON_DIR USD_NVUSD_LIBDIR; do
  value=${!variable:-}
  if [[ -n "${value}" ]]; then
    printf -v "${variable}" '%s' "$(to_windows_path "${value}")"
    export "${variable}"
  fi
done

if [[ -n "${USD_NVUSD_BINDIR:-}" && -n "${USD_NVUSD_LIBDIR:-}" && -n "${USD_LIBPYTHON_DIR:-}" ]]; then
  export PXR_USD_WINDOWS_DLL_PATH="${USD_NVUSD_BINDIR};${USD_NVUSD_LIBDIR};${USD_LIBPYTHON_DIR}"
fi

arguments=()
for argument in "$@"; do
  if [[ "${argument}" == /* ]]; then
    arguments+=("$(to_windows_path "${argument}")")
  else
    arguments+=("${argument}")
  fi
done

WINEDEBUG=-all exec wine "${python_exe}" "${arguments[@]}"
