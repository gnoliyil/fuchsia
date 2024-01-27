#!/bin/bash
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Creates an extended raw FVM image from a source FVM.
#
# Arguments:
#   - Input FVM. The format can be raw or compressed.
#   - output image file
#   - (optional) desired minimum disk-size of the output FVM image, defaults to
#     twice the uncompressed size of the given image. If the requested size is
#     already smaller than the disk-size specified in the metadata, the
#     disk-size will remain the same. It is guaranteed that the file size of the
#     output image is the same as the disk-size in the metadata.
#
# Output:
#   (stderr) error logs on failure.
#
# Returns:
#   0 on success, 1 on failure.
function fx-fvm-extend-image {
  fvm_tool="$1"
  fvm_in="$2"
  fvmimg="$3"

  # Store the decompressed file with a deterministic path to facilitate testing
  "${fvm_tool}" "${fvm_in}.decompressed" decompress --default "${fvm_in}"
  # Rename the decompressed file to |fvmimg| and perform extension.
  mv "${fvm_in}.decompressed" "${fvmimg}"

  stat_flags=()
  if [[ $(uname) == "Darwin" ]]; then
    stat_flags+=("-x")
  fi
  stat_output=$(LC_ALL=C stat "${stat_flags[@]}" "${fvmimg}")
  if [[ "$stat_output" =~ Size:\ ([0-9]+) ]]; then
    size="${BASH_REMATCH[1]}"
    recommended_size=$((size * 2))
    if [[ $# -gt 2 && -n "$4" ]]; then
      newsize=$4
      if [[ "${newsize}" -le "${size}" ]]; then
        fx-error "Image size has to be greater than ${size} bytes.  Recommended value is ${recommended_size} bytes."
        return 1
      fi
    else
      newsize="${recommended_size}"
    fi
     echo >&2 "Creating disk image..."
     "${fvm_tool}" "${fvmimg}" extend --length "${newsize}" --length-is-lowerbound
     echo >&2 "done"
  else
    fx-error "Could not extend FVM, unable to stat FVM image ${fvm_in}"
    return 1
  fi
  return 0
}

# Finds a source FVM to generate a raw FVM image from.
#
# The raw FVM is primarily used by the emulator, and is coverted from other FVM
# formats on-demand. This locate the best source FVM to create the raw FVM from.
#
# The resulting path will generally be passed to fx-fvm-extend-image, but it's
# useful to separate the functions since failing to find an FVM isn't usually
# an error whereas failing to extend one is.
#
# Arguments:
#   None
#
# Output:
#   (stdout) path to a source FVM image if one was found, relative to
#   FUCHSIA_BUILD_DIR. nothing otherwise.
function fx-fvm-find-raw-source {
  # Look for source FVM formats in this order. Every build that uses an FVM
  # should produce at least one of these.
  source_fvms=(
    "$(fx-command-run list-build-artifacts --name storage-full --type blk --allow-empty images)"
    "$(fx-command-run list-build-artifacts --name storage-sparse --type blk --allow-empty images)"
    "$(fx-command-run list-build-artifacts --name fvm.fastboot --type blk --allow-empty images)"
  )

  for source_fvm in "${source_fvms[@]}"; do
    if [[ -n "${source_fvm}" && -f "${FUCHSIA_BUILD_DIR}/${source_fvm}" ]]; then
      echo "${source_fvm}"
      return
    fi
  done
}
