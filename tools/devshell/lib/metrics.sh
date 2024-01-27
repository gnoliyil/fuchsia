#!/bin/bash
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Common methods for metrics collection.
#
# Note: For non-shell programs, use metrics_custom_report.sh instead.
#
# Report events to the metrics collector from `fx`, the `fx metrics`, and the
# rare subcommand that has special metrics needs.
#
# How to use it: This file is sourced in //scripts/fx for tracking command
# execution and in //tools/devshell/metrics for managing the metrics collection
# settings. Developers of shell-based subcommands can source this file in their
# subcommand if they need custom event tracking, but only the method
# track-subcommand-custom-event can be used in this context.
#
# This script assumes that vars.sh has already been sourced, since it
# depends on FUCHSIA_DIR being defined correctly.

_METRICS_GA_PROPERTY_ID="UA-127897021-6"
_METRICS_TRACK_ALL_ARGS=( "emu" "set" "fidlcat" "run-host-tests" )
_METRICS_TRACK_RESULTS=( "set" "build" )
_METRICS_ALLOWS_CUSTOM_REPORTING=( "test" )
# If args match the below, then track capture group 1
_METRICS_TRACK_REGEX=(
    "^run (fuchsia-pkg:\/\/[[:graph:]]*)"
    "^shell (run fuchsia-pkg:\/\/[[:graph:]]*)"
)
# We collect metrics when these operations happen without capturing all of
# their args.
_METRICS_TRACK_COMMAND_OPS=(
    "shell activity"
    "shell basename"
    "shell bssl"
    "shell bt-avdtp-tool"
    "shell bt-avrcp-controller"
    "shell bt-cli"
    "shell bt-hci-emulator"
    "shell bt-hci-tool"
    "shell bt-intel-tool"
    "shell bt-le-central"
    "shell bt-le-peripheral"
    "shell bt-pairing-tool"
    "shell bt-snoop-cli"
    "shell bugreport"
    "shell cal"
    "shell cat"
    "shell catapult_converter"
    "shell cksum"
    "shell cmp"
    "shell cols"
    "shell comm"
    "shell cowsay"
    "shell cp"
    "shell crashpad_database_util"
    "shell cs"
    "shell curl"
    "shell cut"
    "shell date"
    "shell dirname"
    "shell du"
    "shell echo"
    "shell ed"
    "shell env"
    "shell expand"
    "shell expr"
    "shell false"
    "shell far"
    "shell fdio_spawn_helper"
    "shell fdr"
    "shell find"
    "shell fold"
    "shell fuchsia_benchmarks"
    "shell gltf_export"
    "shell grep"
    "shell head"
    "shell hostname"
    "shell input"
    "shell iperf3"
    "shell iquery"
    "shell join"
    "shell josh"
    "shell limbo_client"
    "shell link"
    "shell locate"
    "shell log_listener"
    "shell ls"
    "shell md5sum"
    "shell mediasession_cli_tool"
    "shell mkdir"
    "shell mktemp"
    "shell mv"
    "shell net"
    "shell netdump"
    "shell nl"
    "shell od"
    "shell onet"
    "shell paste"
    "shell pathchk"
    "shell pkgctl"
    "shell pm"
    "shell present_view"
    "shell print_input"
    "shell printenv"
    "shell printf"
    "shell process_input_latency_trace"
    "shell pwd"
    "shell readlink"
    "shell rev"
    "shell rm"
    "shell rmdir"
    "shell run"
    "shell run_simplest_app_benchmark.sh"
    "shell run-test-suite"
    "shell scp"
    "shell screencap"
    "shell sed"
    "shell seq"
    "shell set_renderer_params"
    "shell sh"
    "shell sha1sum"
    "shell sha224sum"
    "shell sha256sum"
    "shell sha384sum"
    "shell sha512-224sum"
    "shell sha512-256sum"
    "shell sha512sum"
    "shell signal_generator"
    "shell sleep"
    "shell snapshot"
    "shell sort"
    "shell split"
    "shell sponge"
    "shell ssh"
    "shell ssh-keygen"
    "shell stash_ctl"
    "shell strings"
    "shell sync"
    "shell system-update-checker"
    "shell tail"
    "shell tar"
    "shell tee"
    "shell test"
    "shell tftp"
    "shell tiles_ctl"
    "shell time"
    "shell touch"
    "shell tr"
    "shell trace"
    "shell true"
    "shell tsort"
    "shell tty"
    "shell uname"
    "shell unexpand"
    "shell uniq"
    "shell unlink"
    "shell update"
    "shell uudecode"
    "shell uuencode"
    "shell vim"
    "shell virtual_audio"
    "shell vol"
    "shell wav_recorder"
    "shell wc"
    "shell which"
    "shell whoami"
    "shell wlan"
    "shell xargs"
    "shell xinstall"
    "shell yes"
)

_METRICS_TRACK_UNKNOWN_OPS=( "shell" )

# These variables need to be global, but readonly (or declare -r) declares new
# variables as local when they are source'd inside a function.
# "declare -g -r" is the right way to handle it, but it is not supported in
# old versions of Bash, particularly in the one in MacOS. The alternative is to
# make them global first via the assignments above and marking they readonly
# later.
readonly _METRICS_GA_PROPERTY_ID _METRICS_TRACK_ALL_ARGS _METRICS_TRACK_RESULTS _METRICS_ALLOWS_CUSTOM_REPORTING _METRICS_TRACK_REGEX _METRICS_TRACK_COMMAND_OPS _METRICS_TRACK_UNKNOWN_OPS

# To properly enable unit testing, METRICS_CONFIG is not read-only
METRICS_CONFIG="${FUCHSIA_DIR}/.fx-metrics-config"

_METRICS_DEBUG=0
_METRICS_DEBUG_LOG_FILE=""
_METRICS_USE_VALIDATION_SERVER=0

INIT_WARNING=$'Please opt in or out of fx metrics collection.\n'
INIT_WARNING+=$'You will receive this warning until an option is selected.\n'
INIT_WARNING+=$'To check what data we collect, run `fx metrics`\n'
INIT_WARNING+=$'To opt in or out, run `fx metrics <enable|disable>\n'

# Each Analytics batch call can send at most this many hits.
declare -r BATCH_SIZE=20
# Keep track of how many hits have accumulated.
hit_count=0
# Holds curl args for the current batch of hits.
curl_args=()

function __is_in {
  local v="$1"
  shift
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == "$v" ]]; then
      return 0
    fi
    shift
  done
  return 1
}

function __is_in_regex {
  local v="$1"
  shift
  while [[ $# -gt 0 ]]; do
    if [[ $v =~ $1 ]]; then
      return 0
    fi
    shift
  done
  return 1
}

function metrics-read-config {
  METRICS_UUID=""
  METRICS_ENABLED=0
  _METRICS_DEBUG_LOG_FILE=""
  if [[ ! -f "${METRICS_CONFIG}" ]]; then
    return 1
  fi
  source "${METRICS_CONFIG}"
  if [[ $METRICS_ENABLED == 1 && -z "$METRICS_UUID" ]]; then
    METRICS_ENABLED=0
    return 1
  fi
  return 0
}

function metrics-write-config {
  local enabled=$1
  if [[ "$enabled" -eq "1" ]]; then
    local uuid="$2"
    if [[ $# -gt 2 ]]; then
      local debug_logfile="$3"
    fi
  fi
  local -r tempfile="$(mktemp)"

  # Exit trap to clean up temp file
  trap "[[ -f \"${tempfile}\" ]] && rm -f \"${tempfile}\"" EXIT

  {
    echo "# Autogenerated config file for fx metrics. Run 'fx help metrics' for more information."
    echo "METRICS_ENABLED=${enabled}"
    echo "METRICS_UUID=\"${uuid}\""
    if [[ -n "${debug_logfile}" ]]; then
      echo "_METRICS_DEBUG_LOG_FILE=${debug_logfile}"
    fi
  } >> "${tempfile}"
  # Only rewrite the config file if content has changed
  if ! cmp --silent "${tempfile}" "${METRICS_CONFIG}" ; then
    mv -f "${tempfile}" "${METRICS_CONFIG}"
  fi
}

function metrics-read-and-validate {
  local hide_init_warning=$1
  if ! metrics-read-config; then
    if [[ $hide_init_warning -ne 1 ]]; then
      fx-warn "${INIT_WARNING}"
    fi
    return 1
  fi
  return 0
}

function metrics-set-debug-logfile {
  _METRICS_DEBUG_LOG_FILE="$1"
  return 0
}

function metrics-get-debug-logfile {
  if [[ -n "$_METRICS_DEBUG_LOG_FILE" ]]; then
    echo "$_METRICS_DEBUG_LOG_FILE"
  fi
}

function metrics-maybe-log {
  local filename="$(metrics-get-debug-logfile)"
  if [[ -n "$filename" ]]; then
    if [[ ! -f "$filename" && -w $(dirname "$filename") ]]; then
      touch "$filename"
    fi
    if [[ -w "$filename" ]]; then
      TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
      echo -n "${TIMESTAMP}:" >> "$filename"
      for i in "$@"; do
        if [[ "$i" =~ ^"--" ]]; then
          continue # Skip switches.
        fi
        # Space before $i is intentional.
        echo -n " $i" >> "$filename"
      done
      # Add a newline at the end.
      echo >> "$filename"
    fi
  fi
}

# Arguments:
#   - the name of the fx subcommand
#   - event action
#   - (optional) event label
function track-subcommand-custom-event {
  local subcommand="$1"
  local event_action="$2"
  shift 2
  local event_label="$*"

  # Only allow custom arguments to subcommands defined in # $_METRICS_ALLOWS_CUSTOM_REPORTING
  if ! __is_in "$subcommand" "${_METRICS_ALLOWS_CUSTOM_REPORTING[@]}"; then
    return 1
  fi

  # Limit to the first 100 characters
  # The Analytics API supports up to 500 bytes, but it is likely that
  # anything larger than 100 characters is an invalid execution and/or not
  # what we want to track.
  event_label=${event_label:0:100}

  local hide_init_warning=1
  metrics-read-and-validate $hide_init_warning
  if [[ $METRICS_ENABLED == 0 ]]; then
    return 0
  fi

  analytics_args=(
    "t=event" \
    "ec=fx_custom_${subcommand}" \
    "ea=${event_action}" \
    "el=${event_label}" \
    )

  _add-to-analytics-batch "${analytics_args[@]}"
  # Send any remaining hits.
  _send-analytics-batch
  return 0
}

# Arguments:
#   - the name of the fx subcommand
#   - args of the subcommand
function track-command-execution {
  local subcommand="$1"
  shift
  local args="$*"
  local subcommand_op;
  if [[ $# -gt 0 ]]; then
    local subcommand_arr=( $args )
    subcommand_op=${subcommand_arr[0]}
  fi

  local hide_init_warning=0
  if [[ "$subcommand" == "metrics" ]]; then
    hide_init_warning=1
  fi
  metrics-read-and-validate $hide_init_warning
  if [[ $METRICS_ENABLED == 0 ]]; then
    return 0
  fi

  if [[ "$subcommand" == "set" ]]; then
    # Add separate fx_set hits for packages
    _process-fx-set-command "$@"
  fi

  # Track arguments to the subcommands in $_METRICS_TRACK_ALL_ARGS
  if __is_in "$subcommand" "${_METRICS_TRACK_ALL_ARGS[@]}"; then
    # Track all arguments.
    # Limit to the first 100 characters of arguments.
    # The Analytics API supports up to 500 bytes, but it is likely that
    # anything larger than 100 characters is an invalid execution and/or not
    # what we want to track.
    args=${args:0:100}
  elif __is_in_regex "${subcommand} ${args}" "${_METRICS_TRACK_REGEX[@]}"; then
    args="${BASH_REMATCH[1]}"
  elif [ -n "${subcommand_op}" ]; then
    if __is_in "${subcommand} ${subcommand_op}" \
        "${_METRICS_TRACK_COMMAND_OPS[@]}"; then
      # Track specific subcommand arguments (instead of all of them)
      args="${subcommand_op}"
    elif __is_in "${subcommand}" "${_METRICS_TRACK_UNKNOWN_OPS[@]}"; then
      # We care about the fact there was a subcommand_op, but we haven't
      # explicitly opted it into metrics collection.
      args="\$unknown_subcommand"
    else
      args=""
    fi
  else
    # Track no arguments
    args=""
  fi

  analytics_args=(
    "t=event" \
    "ec=fx" \
    "ea=${subcommand}" \
    "el=${args}" \
    )

  _add-to-analytics-batch "${analytics_args[@]}"
  # Send any remaining hits.
  _send-analytics-batch
  return 0
}

# Arguments:
#   - args of `fx set`
function _process-fx-set-command {
  local target="$1"
  shift
  while [[ $# -ne 0 ]]; do
    case $1 in
      --with)
        shift # remove "--with"
        _add-fx-set-hit "$target" "fx-with" "$1"
        ;;
      --with-base)
        shift # remove "--with-base"
        _add-fx-set-hit "$target" "fx-with-base" "$1"
        ;;
      *)
        ;;
    esac
    shift
  done
}

# Arguments:
#   - the product.board target for `fx set`
#   - category name, either "fx-with" or "fx-with-base"
#   - package(s) following "--with" or "--with-base" switch
function _add-fx-set-hit {
  target="$1"
  category="$2"
  packages="$3"
  # Packages argument can be a comma-separated list.
  IFS=',' read -ra packages_parts <<< "$packages"
  for p in "${packages_parts[@]}"; do
    analytics_args=(
      "t=event" \
      "ec=${category}" \
      "ea=${p}" \
      "el=${target}" \
      )

    _add-to-analytics-batch "${analytics_args[@]}"
  done
}

# Arguments:
#   - time taken to complete (milliseconds)
#   - exit status
#   - the name of the fx subcommand
#   - args of the subcommand
function track-command-finished {
  timing=$1
  exit_status=$2
  subcommand=$3
  shift 3
  args="$*"

  metrics-read-config
  if [[ $METRICS_ENABLED == 0 ]] || ! __is_in "$subcommand" "${_METRICS_TRACK_RESULTS[@]}"; then
    return 0
  fi

  # Only track arguments to the subcommands in $_METRICS_TRACK_ALL_ARGS
  if ! __is_in "$subcommand" "${_METRICS_TRACK_ALL_ARGS[@]}"; then
    args=""
  else
    # Limit to the first 100 characters of arguments.
    # The Analytics API supports up to 500 bytes, but it is likely that
    # anything larger than 100 characters is an invalid execution and/or not
    # what we want to track.
    args=${args:0:100}
  fi

  if [[ $exit_status == 0 ]]; then
    # Successes are logged as timing hits
    hit_type="timing"
    analytics_args=(
      "t=timing" \
      "utc=fx" \
      "utv=${subcommand}" \
      "utt=${timing}" \
      "utl=${args}" \
      )
  else
    # Failures are logged as event hits with a separate category
    # exit status is stored as Custom Dimension 1
    hit_type="event"
    analytics_args=(
      "t=event" \
      "ec=fx_exception" \
      "ea=${subcommand}" \
      "el=${args}" \
      "cd1=${exit_status}" \
      )
  fi

  _add-to-analytics-batch "${analytics_args[@]}"
  # Send any remaining hits.
  _send-analytics-batch
  return 0
}

# Add an analytics hit with the given args to the batch of hits. This will trigger
# sending a batch when the batch size limit is hit.
#
# Arguments:
#   - analytics arguments, e.g. "t=event" "ec=fx" etc.
function _add-to-analytics-batch {
  if [[ $# -eq 0 ]]; then
    return 0
  fi

  if (( hit_count > 0 )); then
    # Each hit in a batch must be on its own line. The below will append a newline
    # without url-encoding it. Note that this does add a '&' to the end of each hit,
    # but those are ignored by Google Analytics.
    curl_args+=(--data-binary)
    curl_args+=($'\n')
  fi

  # All hits send some common parameters
  local app_name="$(_app_name)"
  local app_version="$(_app_version)"
  params=(
    "v=1" \
    "tid=${_METRICS_GA_PROPERTY_ID}" \
    "cid=${METRICS_UUID}" \
    "an=${app_name}" \
    "av=${app_version}" \
    "$@" \
    )
  for p in "${params[@]}"; do
    curl_args+=(--data-urlencode)
    curl_args+=("$p")
  done

  : $(( hit_count += 1 ))
  if ((hit_count == BATCH_SIZE)); then
    _send-analytics-batch
  fi
}

# Sends the current batch of hits to the Analytics server. As a side effect, clears
# the hit count and batch data.
function _send-analytics-batch {
  if [[ $hit_count -eq 0 ]]; then
    return 0
  fi

  local user_agent="Fuchsia-fx $(_os_data)"
  local url_path="/batch"
  local result=""
  if [[ $_METRICS_DEBUG == 1 && $_METRICS_USE_VALIDATION_SERVER == 1 ]]; then
    url_path="/debug/collect"
    # Validation server does not accept batches. Send just the first hit instead.
    local limit=0
    for i in "${curl_args[@]}"; do
      if [[ "$i" == "--data-binary" ]]; then
        curl_args=("${curl_args[@]:0:$limit}")
        break
      fi
      : $(( limit += 1 ))
    done
  fi
  if [[ $_METRICS_DEBUG == 1 && $_METRICS_USE_VALIDATION_SERVER == 0 ]]; then
    # if testing and not using the validation server, always return 202
    result="202"
  elif [[ $_METRICS_DEBUG == 0 || $_METRICS_USE_VALIDATION_SERVER == 1 ]]; then
    result=$(curl -s -o /dev/null -w "%{http_code}" "${curl_args[@]}" \
      -H "User-Agent: $user_agent" \
      "https://www.google-analytics.com/${url_path}")
  fi
  metrics-maybe-log "${curl_args[@]}" "RESULT=${result}"

  # Clear batch.
  hit_count=0
  curl_args=()
}

function _os_data {
  if command -v uname >/dev/null 2>&1 ; then
    uname -rs
  else
    echo "Unknown"
  fi
}

function _app_name {
  if [[ -n "${BASH_VERSION}" ]]; then
    echo "bash"
  elif [[ -n "${ZSH_VERSION}" ]]; then
    echo "zsh"
  else
    echo "Unknown"
  fi
}

function _app_version {
  if [[ -n "${BASH_VERSION}" ]]; then
    echo "${BASH_VERSION}"
  elif [[ -n "${ZSH_VERSION}" ]]; then
    echo "${ZSH_VERSION}"
  else
    echo "Unknown"
  fi
}

# Args:
#   debug_log_file: string with a filename to save logs
#   use_validation_hit_server:
#          0 do not hit any Analytics server (for local tests)
#          1 use the Analytics validation Hit server (for integration tests)
#   config_file: string with a filename to save the config file. Defaults to
#          METRICS_CONFIG
function _enable_testing {
  _METRICS_DEBUG_LOG_FILE="$1"
  _METRICS_USE_VALIDATION_SERVER=$2
  if [[ $# -gt 2 ]]; then
    METRICS_CONFIG="$3"
  fi
  _METRICS_DEBUG=1
  METRICS_UUID="TEST"
}
