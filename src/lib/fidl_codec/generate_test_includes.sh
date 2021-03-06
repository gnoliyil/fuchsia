#!/bin/sh
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This file takes an output file as its first parameter, followed by one or more
# FIDL schema files in JSON format.  It then generates a header containing a C++
# map from schema filename to file contents, which can then be used for test
# purposes.

set -e

readonly OUTPUT_FILE="${1}"
rm -f "${OUTPUT_FILE}"

cat > "${OUTPUT_FILE}" << EOF
#include <map>
#include <string>
// Autogenerated: Do not modify!
namespace fidl_codec_test {
class ${2} {
 public:
  ${2}() {
    map_ = {
EOF

add_entry() {
  if [ ! -f "$1" ]; then
    echo "file $1 not found"
    exit 1
  fi
  cat >> "${OUTPUT_FILE}" << EOF
      {"$1", R"FIDL($(cat "$1"))FIDL"},
EOF
}

if [ ${3} = "-content" ]; then
  readonly JSONS_FILE="${4}"
  readonly DEPFILE="${5}"

  if [ ! -f ${JSONS_FILE} ]; then
    echo "file ${JSONS_FILE} not found"
    exit 1
  fi

  readonly JSONS="$(cat "${JSONS_FILE}")"

  for line in $JSONS; do
    add_entry $line
  done

  {
    printf "%s:" "${OUTPUT_FILE}"
    for line in $JSONS; do
      if [ -n "${line}" ]; then
        printf " %s" "${line}"
      fi
    done
  } > "${DEPFILE}"
else
  shift 2

  for i in "$@"; do
    add_entry ${i}
  done
fi

cat >> "${OUTPUT_FILE}" << EOF
    };
  }
  std::map<std::string, std::string> &map() { return map_; }
 private:
  std::map<std::string, std::string> map_;
};

}  // namespace fidl_codec_test
EOF
