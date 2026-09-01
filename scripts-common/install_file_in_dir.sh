#!/usr/bin/env bash

set -e
shopt -s nullglob

sources=( $1 )
destination=$2

if (( ${#sources[@]} == 0 )); then
  echo "ERROR: no files matched '$1'" >&2
  exit 1
fi

mkdir -p "${destination}"
cp -f "${sources[@]}" "${destination}"
