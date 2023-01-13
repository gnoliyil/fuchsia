#!/bin/bash
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e -o pipefail

readonly OUTPUT="$1"
readonly RSPFILE="$2"
readonly DEPFILE="$3"

DEP_FILES=()

generate() {
  echo "\
// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is generated by: $0 $*
// DO NOT EDIT!
"
  read_rspfile < "$RSPFILE"
}

read_rspfile() {
  local file stamp
  while read file; do
    # It starts with either ".../foo.unstripped/" or exactly "foo.unstripped/".
    stripped_file="${file/\/*.unstripped/}"
    stripped_file="${stripped_file#*.unstripped/}"
    stamp="${stripped_file}.build-id.stamp"
    DEP_FILES+=("$stamp")
    generate_one "$file" "$(<"$stamp")"
  done
}

generate_one() {
  local file="$1" id="$2" soname= id_bytes=
  if [[ "$file" == *.so ]]; then
    soname="${file##*/}"
  fi
  while [ -n "$id" ]; do
    id_bytes+="0x${id:0:2},"
    id="${id:2}"
  done
  echo "MakeElfId({$id_bytes}, \"$soname\"),"
}

generate > "$OUTPUT"

# Note that generate updates DEP_FILES by side-effect, so this must be last!
echo "$OUTPUT:" "${DEP_FILES[@]}" > "$DEPFILE"
