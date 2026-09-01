#!/usr/bin/env bash

set -e

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WINEDEBUG=-all exec wine "${script_dir}/../external/glslangvalidator/glslangValidator.exe" "$@"
