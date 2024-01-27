#!/bin/bash
# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# See usage() for description.

set -uo pipefail

script="$0"
script_dir="$(dirname "$script")"

source "$script_dir"/common-setup.sh


######################## DEPRECATION NOTICE ###################################
python_rel="$(relpath . "$python")"
script_dir_rel="$(relpath . "$script_dir")"
replacement_script="$script_dir_rel/output_leak_scanner.py"
equivalent_command=(
  "$python_rel" -S "$replacement_script" "${@:1}"
)
cat <<EOF
NOTICE: $0 is deprecated and will be removed.
Instead, use $replacement_script with the same args:
  ${equivalent_command[@]}
EOF
# one could just do this here:
# exec "${equivalent_command[@]}"
##################### end of DEPRECATION NOTICE ###############################


project_root="$default_project_root"

build_subdir="$(relpath "$project_root" . )"

function usage() {
  cat <<EOF
This wrapper script validates a command in the following manner:

Reject any occurrences of the output dir ($build_subdir):

  1) in the command's tokens
  2) in the output files' paths
  3) in the output files' contents

Usage: $script [options] output_files... -- command...
Options:
  --help | -h : print help and exit
  --label STRING : build system label for this action
  --[no-]execute : check the command without executing it.  [default: execute]
EOF
}

command_break=0
prev_opt=
label=
execute=1
# Collect names of output files.
outputs=()
for opt
do
  # handle --option arg
  if test -n "$prev_opt"
  then
    eval "$prev_opt"=\$opt
    prev_opt=
    shift
    continue
  fi

  # Extract optarg from --opt=optarg
  optarg=
  case "$opt" in
    -*=*) optarg="${opt#*=}" ;;  # remove-prefix, shortest-match
  esac

  case "$opt" in
    -h | --help) usage; exit ;;
    --label) prev_opt=label ;;
    --label=*) label="$optarg" ;;
    --execute) execute=1 ;;
    --no-execute) execute=0 ;;
    --) command_break=1; shift; break ;;
    *) outputs+=( "$opt" ) ;;
  esac
  shift
done

label_diagnostic=
test -z "$label" || label_diagnostic=" [$label]"

function error_msg() {
  echo "[$script]$label_diagnostic Error: " "$@" "(See http://go/remotely-cacheable for more information.)"
}

test "$command_break" = 1 || {
  error_msg "Missing -- before command."
  usage
  exit 1
}

# Scan outputs' paths
err=0
if [[ "$build_subdir" != "." ]]
then
  for f in "${outputs[@]}"
  do
    # Match paths on \<whole-word\> boundaries.
    # Unfortunately, built-in bash regex not sufficient.
    if echo "$f" | grep -q "\<$build_subdir\>" ; then
        err=1
        error_msg "Output path '$f' contains '$build_subdir'." \
          "Adding rebase_path(..., root_build_dir) may fix this to be relative." \
          "If this command requires an absolute path, mark this action in GN with 'no_output_dir_leaks = false'."
    fi
  done

  # Command is in "$@".  Scan its tokens for $build_dir.
  for tok
  do
    case "$tok" in
      # C++: a few clang/gcc flags remap paths, and thus expect the self path
      # as part of the option argument.
      -fdebug-prefix-map=*"$build_subdir"* | \
      -ffile-prefix-map=*"$build_subdir"* | \
      -fmacro-prefix-map=*"$build_subdir"* | \
      -fcoverage-prefix-map=*"$build_subdir"* )
        ;;
      *"$build_subdir"* )
        # Check with whole-word boundaries.
        if echo "$tok" | grep -q "\<$build_subdir\>" ; then
          err=1
          error_msg "Command token '$tok' contains '$build_subdir'." \
            "Adding rebase_path(..., root_build_dir) may fix this to be relative." \
            "If this command requires an absolute path, mark this action in GN with 'no_output_dir_leaks = false'."
        fi
        ;;
    esac
    # Do not shift, keep tokens for execution.
  done
fi

if [[ "$execute" == 1 ]]
then
  # Run the command.
  "$@"

  status="$?"

  # On success, check the output files' contents.
  test "$status" != 0 || {
    for f in "${outputs[@]}"
    do
      # skip directory outputs
      if test -f "$f"
      then
        if grep -qwF "\<$build_subdir\>" "$f"
        then
          err=1
          error_msg "Output file $f contains '$build_subdir'." \
            "If this cannot be fixed in the tool, mark this action in GN with 'no_output_dir_leaks = false'."
        fi
        # TODO(http://fxbug.dev/92670) check for known remote paths, like "/b/f/w"
      fi
    done
  }
else
  # Skip execution.
  status=0
fi

test "$err" = 0 || exit 1

exit "$status"
