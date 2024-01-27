# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

function package-server-mode {
  # Next try to determine what server mode we should actually be using.
  local mode=$(fx-command-run ffx config get repository.server.mode 2>/dev/null)
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "Unable to get the configured repository server mode"
    return "${err}"
  fi

  # If unspecified, default to using `pm`.
  if [[ "${mode}" = "" || "${mode}" = "null" ]]; then
    mode="pm"
  else
    # Regex: Remove the leading and trailing quotes from the server mode.
    if [[ $mode =~ \"(.*)\" ]]; then
      mode="${BASH_REMATCH[1]}"
    else
      fx-error "could not parse ffx server mode: ${mode}"
      return 1
    fi
  fi

  # Check that the server mode is known.
  case "${mode}" in
    "pm") echo pm ;;
    "ffx") echo ffx ;;
    *)
      fx-error "Unknown repository server mode: ${mode}"
      return 1
  esac

  return 0
}

# Determines if a package server can be started.
#
# This depends on our package server mode:
#
# * If we are using pm, return 0 if the specified port is available.
# * If we are using ffx, return 0 if ffx is configured to listen on the
#   specified port.
function check-if-we-can-start-package-server {
  local mode=$1
  local expected_ip=$2
  local expected_port=$3

  # Checks if a pm package server process appears to be running.
  if [[ -z "$(pgrep -f 'pm serve')" ]]; then
    local is_pm_running=1 # 1 means that pm is not running
  else
    local is_pm_running=0 # 0 means that pm is running
  fi

  # Check that the server mode is known, and that we have the ability to use this server mode.
  if [[ "${mode}" = "pm" ]]; then
    # We can run if nothing is listening on our port.
    if ! is-listening-on-port "${expected_port}"; then
      return 0
    fi

    # Something is using the port. Try to determine if it's another pm server, or ffx.
    if [[ "${is_pm_running}" -eq 0 ]]; then
      fx-error "It looks like another \"fx serve-updates\" process may be running."
      fx-error "It may be the ffx repository server. Try shutting it down with:"
      fx-error ""
      fx-error "$ ffx repository server stop"
      fx-error ""
      fx-error "Otherwise, try stopping that process, and re-run \"fx serve\"."
      return 1
    fi

    configured_mode="$(package-server-mode)"
    if [[ "${configured_mode}" == "ffx" ]]; then
      fx-error "Even though we are trying to start a pm package server, it appears"
      fx-error "we are configured to use the ffx repository server. Try shutting it"
      fx-error "down with:"
      fx-error ""
      fx-error "$ ffx repository server stop"
      fx-error "$ ffx config set repository.server.mode pm"
      fx-error "$ fx serve"
      return 1
    else
      # Check if the ffx package repository server is enabled. If so, shut it
      # down if it's configured to use the port we're trying to use.
      local ffx_enabled=$(ffx-repository-server-enabled)
      local err=$?
      if [[ "${err}" -eq 0 && "${ffx_enabled}" == "true" ]]; then
        local ffx_port=$(ffx-repository-server-port)
        err=$?
        if [[ "${err}" -eq 0 && "${ffx_port}" == "${expected_port}" ]]; then
          fx-warn "The ffx repository server may be running, and is configured"
          fx-warn "to run on ${expected_port}. Trying to shut it down..."

          fx-command-run ffx repository server stop
          err=$?
          if [[ "${err}" -ne 0 ]]; then
            fx-warn "Failed to stop ffx repository server. Checking if the port"
            fx-warn "freed up anyway."
          fi

          if ! is-listening-on-port "${expected_port}"; then
            fx-info "ffx repository server shut down"
            return 0
          fi
        fi
      fi

      fx-error "It looks like some process is listening on ${port}."
      fx-error "You probably need to stop that and start a new one here with \"fx serve\""
    fi

    return 1
  else
    # Make sure ffx repository is configured to listen on this address.
    ffx-repository-check-server-address "${expected_ip}" "${expected_port}"
    local err=$?
    if [[ "${err}" -ne 0 ]]; then
      return 1
    fi

    # We can run if nothing is listening on our port.
    if is-listening-on-port "${expected_port}"; then
      local ffx_addr=$(ffx-repository-server-address)

      fx-error "Another process is using port '${expected_port}', which"
      fx-error "will block the ffx repository server from listening on ${ffx_addr}."
      fx-error ""
      fx-error "Try shutting down that process, and re-running \"fx serve\"."

      return 1
    fi

    return 0
  fi
}

function check-for-package-server {
  local mode="$(package-server-mode)"
  local err=$?
  if [[ "${err}" -ne 0 ]]; then
    return 1
  fi

  if [[ "${mode}" = "pm" ]]; then
    # Make sure it is running.
    if [[ -z "$(pgrep -f 'pm serve .*/amber-files')" ]]; then
      fx-error "It looks like serve-updates is not running."
      fx-error "You probably need to start \"fx serve\""
      return 1
    fi

    # Warn if it is using the wrong repository.
    if [[ -z "$(pgrep -f "pm serve .*${FUCHSIA_BUILD_DIR}/amber-files")" ]]; then
      fx-warn "WARNING: It looks like serve-updates is running in a different workspace."
      fx-warn "WARNING: You probably need to stop that one and start a new one here with \"fx serve\""
    fi

    # Warn if incremental is enabled for this shell, but the server is not auto publishing packages.
    if is_feature_enabled "incremental"; then
      # Regex terminates with a space to avoid matching the -persist option.
      if [[ -z "$(pgrep -f "pm serve .*${FUCHSIA_BUILD_DIR}/amber-files .*-p ")" ]]; then
        fx-warn "WARNING: Incremental build is enabled, but it looks like incremental build is disabled for serve-updates."
        fx-warn "WARNING: You probably need to stop serve-updates, and restart it with incremental build enabled."
        fx-warn "WARNING: You can enable incremental build in the shell running serve-updates with 'export FUCHSIA_DISABLED_incremental=0'"
      fi
    fi
  else
    local ffx_addr=$(ffx-repository-server-address)
    local err=$?
    if [[ "${err}" -ne 0 ]]; then
      return $err
    fi

    if [[ -z "${ffx_addr}" ]]; then
      fx-error "repository server is currently disabled. to re-enable, run:"
      fx-error ""
      fx-error "$ ffx config set repository.server.mode ffx"
      fx-error "$ ffx config set repository.server.listen \"[::]:8083\""
      fx-error ""
      fx-error "Then re-run this command."
      return 1
    fi

    # Regex: zero or more characters, a colon, then at least one digit, matching
    # the digits.
    if [[ ${ffx_addr} =~ .*:([0-9]+) ]]; then
      local ffx_port="${BASH_REMATCH[1]}"
    else
      fx-error "could not parse ip and port from ffx server address: $actual_addr"
      return 1
    fi

    if [[ "${ffx_port}" -eq 0 ]]; then
      fx-warn "WARNING: The server is configured to listen on a random port."
      fx-warn "WARNING: We can't determine which port this is, so assuming it's running."
    else
      if ! is-listening-on-port "${ffx_port}"; then
        fx-error "It looks like the ffx package server is not running."
        fx-error "You probably need to run \"fx add-update-source\""
        return 1
      fi
    fi

    # FIXME(http://fxbug.dev/80431): Check if the current `devhost` points at
    # '${FUCHSIA_BUILD_DIR}/amber-files'.
  fi

  return 0
}

function is-listening-on-port {
  local port=$1

  if [[ "$(uname -s)" == "Darwin" ]]; then
    if netstat -anp tcp | awk '{print $4}' | grep "\.${port}$" > /dev/null; then
      return 0
    fi
  else
    if ss -f inet -f inet6 -an | awk '{print $5}' | grep ":${port}$" > /dev/null; then
      return 0
    fi
  fi

  return 1
}

function ffx-default-repository-name {
    # Use the build directory's name by default. Note that package URLs are not
    # allowed to have underscores, so replace them with hyphens.
    basename "${FUCHSIA_BUILD_DIR}" | tr '_' '-'
}

function ffx-start-server {
  fx-command-run ffx repository server start
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "The repository server was unable to be started"
    return "${err}"
  fi

  return 0
}

function ffx-stop-server {
  fx-command-run ffx repository server stop
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "The repository server was unable to be stopped"
    return "${err}"
  fi

  return 0
}

function ffx-add-repository {
  local repo_name=$1
  shift

  if [[ -z "${repo_name}" ]]; then
    fx-error "The repository name was not specified"
    return 1
  fi

  fx-command-run ffx repository add-from-pm \
    --repository "${repo_name}" \
    "${FUCHSIA_BUILD_DIR}/amber-files"
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "The repository was not able to be added to ffx"
    return $err
  fi

  return 0
}

function ffx-register-repository {
  local repo_name=$1
  shift

  ffx-add-repository "${repo_name}" || return $?

  # Start the server before registering a repository. While `register`
  # technically also automatically starts the server in the background, running
  # it in the foreground gives us better error messages.
  ffx-start-server || return $?

  # FIXME(http://fxbug.dev/98589): ffx cannot yet parse targets that may
  # include the ssh port. We'll explicitly specify the target here with
  # `get-device-name`, which strips out the port if present.
  fx-command-run ffx \
    --target "$(get-device-name)" \
    target repository register \
    --repository "${repo_name}" \
    --alias "fuchsia.com" \
    --alias "chromium.org" \
    "$@"
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "Could not register the package repository on the target device"
    fx-error ""
    fx-error "If you recently enabled the ffx server, you may need to start"
    fx-error "the ffx repository server with"
    fx-error ""
    fx-error "$ ffx repository server start"

    return $err
  fi

  return 0
}

function ffx-repository-server-enabled {
  local enabled=$(fx-command-run ffx config get repository.server.enabled)
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "Unable to get the configured repository server enabled."
    return "${err}"
  fi

  case "${enabled}" in
    "true") echo true ;;
    "false") echo false ;;
    "null") echo false ;;
    *)
      fx-error "Unknown repository.server.enabled: ${enabled}"
      return 1
  esac

  return 0
}

function ffx-repository-server-address {
  local addr=$(fx-command-run ffx config get repository.server.listen)
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "Unable to get the configured repository server address."
    return "${err}"
  fi

  if [[ "${addr}" = "null" ]]; then
    echo ""
  else
    # Regex: Remove the leading and trailing quotes from the address.
    if [[ $addr =~ \"(.*)\" ]]; then
      echo "${BASH_REMATCH[1]}"
    else
      fx-error "could not parse ffx server address: ${addr}"
      return 1
    fi
  fi

  return 0
}

function ffx-repository-server-port {
  local addr=$(ffx-repository-server-address)
  err=$?
  if [[ "${err}" -ne 0 ]]; then
    fx-error "Unable to get the configured repository server address."
    return "${err}"
  fi

  if [[ ${addr} =~ .*:([0-9]+) ]]; then
    echo "${BASH_REMATCH[1]}"
  else
    fx-error "could not parse port from ffx server address: $addr"
    return 1
  fi

  return 0
}

function ffx-repository-check-server-address {
  local expected_ip=$1
  local expected_port=$2

  local actual_addr=$(ffx-repository-server-address)
  local err=$?
  if [[ "${err}" -ne 0 ]]; then
    return "${err}"
  fi

  if [[ -z "${actual_addr}" ]]; then
    fx-error "repository server is currently disabled. to re-enable, run:"
    fx-error ""
    fx-error "$ ffx config set repository.server.listen \"[::]:8083\""
    fx-error ""
    fx-error "Then re-run this command."
    return 1
  fi

  if [[ $actual_addr =~ (.*):([0-9]+) ]]; then
    local actual_ip="${BASH_REMATCH[1]}"
    local actual_port="${BASH_REMATCH[2]}"
  else
    fx-error "could not parse ip and port from ffx server address: $actual_addr"
    return 1
  fi

  if [[ -z "${expected_ip}" ]]; then
    expected_ip="${actual_ip}"
  elif [[ ${expected_ip} =~ : ]]; then
    expected_ip="[${expected_ip}]"
  fi

  if [[ -z "${expected_port}" ]]; then
    expected_port="${actual_port}"
  fi

  local expected_addr="${expected_ip}:${expected_port}"

  if [[ "${expected_addr}" != "${actual_addr}" ]]; then
    fx-error "The repository server is configured to use \"${actual_addr}\", not \"${expected_addr}\""
    fx-error "To switch to a different address, run:"
    fx-error ""
    fx-error "$ ffx config set repository.server.listen \"${expected_addr}\""
    fx-error ""
    fx-error "Then re-run this command."
    fx-error ""
    fx-error "Note: this will change the address for all repositories served by ffx"
    return 1
  fi

  return 0
}

function default-repository-url {
  local mode="$(package-server-mode)"
  local err=$?

  if [[ "${err}" -ne 0 ]]; then
    return 1
  fi

  if [[ "${mode}" = "pm" ]]; then
    echo "fuchsia-pkg://devhost"
  else
    local ffx_repo="$(ffx-default-repository-name)" || return $?
    echo "fuchsia-pkg://${ffx_repo}"
  fi

  return 0
}
