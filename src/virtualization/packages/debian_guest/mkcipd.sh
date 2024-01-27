#!/usr/bin/env bash

# Copyright 2019 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

set -eo pipefail

declare -r DEBIAN_GUEST_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
declare -r FUCHSIA_DIR=$(git rev-parse --show-toplevel)
declare -r CIPD="${FUCHSIA_DIR}/.jiri_root/bin/cipd"

case "${1}" in
arm64)
  ARCH=${1};;
x64)
  ARCH=${1};;
*)
  echo "usage: ${0} {arm64, x64}"
  exit 1;;
esac

# Ensure we are logged in.
if [[ "$(${CIPD} acl-check fuchsia_internal -writer)" == *"doesn't"* ]]; then
  ${CIPD} auth-login
fi

# Clean the existing images directory.
declare -r IMAGE_DIR="${FUCHSIA_DIR}/prebuilt/virtualization/packages/debian_guest/images/${ARCH}"
rm -rf "${IMAGE_DIR}"
mkdir -p "${IMAGE_DIR}"

# Clean the tests source directory.
declare -r TESTS_DIR="/tmp/linux-tests"
rm -rf "${TESTS_DIR}"

# Build Debian.
"${DEBIAN_GUEST_DIR}"/build-image.sh "${IMAGE_DIR}" "${ARCH}" "${@:2}"

declare -r CIPD_PATH="fuchsia_internal/linux/debian_guest-${ARCH}"
${CIPD} create \
    -in "${IMAGE_DIR}" \
    -name "${CIPD_PATH}" \
    -install-mode copy
