#!/bin/bash
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script is to be sourced by other scripts that live in
# the same dir as this one, and should not executed by itself.
# Since this script is to be sourced at the beginning,
# it does not assume or require any other variables to be predefined.

# By sourcing this script, the following symbols are defined:
#   default_project_root (variable)
#   msg (function)
#   relpath (function)
#   timetrace (function)

# This script caches some computed values in environment variables
# to avoid repetitive work in related scripts.
# Environment variables used and exported by this script:
#   _FUCHSIA_RBE_CACHE_VAR_relpath_uses
#   _FUCHSIA_RBE_CACHE_VAR_host_os
#   _FUCHSIA_RBE_CACHE_VAR_host_arch

script="$0"  # This is the name of the invoking script, not this one.
script_basename="$(basename "$script")"
script_dir="$(dirname "$script")"

function msg() {
  echo "[$script_basename]: $@"
}

function timetrace() {
  # Uncomment one of the following:

  # $EPOCHREALTIME has microsecond resolution and is is available in bash 5.0+
  # This is preferred, as it incurs the least measurement overhead.
  msg "[@$EPOCHREALTIME]" "$@"

  # exec'ing date can be slower, ~2ms
  # msg "[@$(date +%H:%M:%S.%N)]" "$@"

  # leave only this line uncommented to quickly disable all calls:
  :
}

# This should point to $FUCHSIA_DIR for the Fuchsia project.
# ../../ because this script lives in build/rbe.
# The value is an absolute path.
readonly default_project_root="$(readlink -f "$script_dir"/../..)"

function _check_realpath_works_for_relative_paths() {
  if which realpath
  then
    # test if it is usable for calculating relative paths
    realpath -s --relative-to=.. "$script" 2>&1 > /dev/null || return 1
    return 0  # success
  else
    # realpath doesn't even exist
    return 1
  fi 2>&1 > /dev/null
}

[[ -n "${_FUCHSIA_RBE_CACHE_VAR_relpath_uses+x}" ]] || {
  # realpath doesn't ship with Mac OS X (provided by coreutils package).
  # We only want it for calculating relative paths.
  # Work around this using Python as needed.
  if _check_realpath_works_for_relative_paths
  then
    export _FUCHSIA_RBE_CACHE_VAR_relpath_uses=realpath
  else
    # Point to our prebuilt python3.
    python="$(ls "$default_project_root"/prebuilt/third_party/python3/*/bin/python3)" || {
      echo "*** Python interpreter not found under $default_project_root/prebuilt/third_party/python3."
      exit 1
    }
    export _FUCHSIA_RBE_CACHE_VAR_relpath_uses="$python"
  fi
}

# By this point, _FUCHSIA_RBE_CACHE_VAR_relpath_uses is definitely set.
case "$_FUCHSIA_RBE_CACHE_VAR_relpath_uses" in
  realpath)
    function relpath() {
      local -r from="$1"
      local -r to="$2"
      # Preserve symlinks.
      realpath -s --relative-to="$from" "$to"
    }
    ;;
  *python*)
    function relpath() {
      local -r from="$1"
      local -r to="$2"
      local -r python="$_FUCHSIA_RBE_CACHE_VAR_relpath_uses"
      "$python" -c "import os; print(os.path.relpath('$to', start='$from'))"
    }
    ;;
esac

[[ -n "${_FUCHSIA_RBE_CACHE_VAR_host_os+x}" ]] || {
  # This is cached to avoid repeating calls to uname.
  detected_os="$(uname -s)"
  case "$detected_os" in
    Darwin) export _FUCHSIA_RBE_CACHE_VAR_host_os="mac" ;;
    Linux) export _FUCHSIA_RBE_CACHE_VAR_host_os="linux" ;;
    *) echo >&2 "Unknown operating system: $detected_os" ; exit 1 ;;
  esac
}

[[ -n "${_FUCHSIA_RBE_CACHE_VAR_host_arch+x}" ]] || {
  # This is cached to avoid repeating calls to uname.
  detected_arch="$(uname -m)"
  case "$detected_arch" in
    x86_64) export _FUCHSIA_RBE_CACHE_VAR_host_arch="x64" ;;
    arm64) export _FUCHSIA_RBE_CACHE_VAR_host_arch="arm64" ;;
    *) echo >&2 "Unknown machine architecture: $detected_arch" ; exit 1 ;;
  esac
}
