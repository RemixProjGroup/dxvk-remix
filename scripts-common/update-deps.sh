#!/usr/bin/env bash

set -e

input_file=$(realpath "$1")
output_file=$(realpath -m "$2")
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

"${script_dir}/packman/packman" pull "${input_file}"
python3 "${script_dir}/patch-tbb.py" "$(dirname "${input_file}")/external/nv_usd_release"
echo "Successfully updated deps" > "${output_file}"
