#!/usr/bin/env bash
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

readonly SCRIPT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly FUCHSIA_ROOT="${SCRIPT_ROOT}/../.."
readonly _ERROR_NO_KEY=112
readonly _ERROR_MISMATCHED_KEYS=113

description="${USER}@$(hostname -f) Generated by Fuchsia tree on $(date)"
no_new_key=false

if [[ "$1" == "--no-new-key" ]]; then
  no_new_key=true
elif [[ "$1" == "--description" ]]; then
  shift
  description="${description}: $@"
fi

function copy_ssh_keys {
  local orig_key dest_key orig_auth_keys dest_auth_keys dest_dir
  orig_key="$1"
  dest_key="$2"
  orig_auth_keys="$3"
  dest_auth_keys="$4"

  dest_dir="$(dirname "${dest_key}")"

  (
    # force subshell to limit scope of umask
    umask 077 && mkdir -p "${dest_dir}"
    umask 177
    cp "${orig_key}" "${dest_key}"
    umask 133

    # Documentation previously recommended copying default SSH identity files
    # (private keys) and authorized_keys files, but not public key files. When
    # missing, these are skipped.
    if [[ -f "${orig_key}.pub" ]]; then
      cp "${orig_key}.pub" "${dest_key}.pub"
    fi
    cat "${orig_auth_keys}" >>"${dest_auth_keys}"
  )
}

function mismatched_keys {
  cat 1>&2 <<EOF
ERROR: You have different Fuchsia SSH credentials in HOME and in FUCHSIA_DIR.

- If you most recently successfully paved a device from the Fuchsia tree
  (fx pave), the SSH credentials in your Fuchsia tree are probably more
  relevant. In this case, backup and delete Fuchsia SSH keys in \$HOME/.ssh/fuchsia_*

- If you most recently successfully paved a device outside the Fuchsia tree,
  the SSH credentials in \$HOME/.ssh are
  probably more relevant. In this case, backup and delete Fuchsia SSH keys in
  FUCHSIA_DIR/.ssh/pkey*

After removing one of the two credentials, execute //tools/ssh-keys/gen-ssh-keys.sh
so that they can be synchronized.
EOF
}

function normalize_local_keys {
  local intree="${FUCHSIA_ROOT}/.ssh/pkey"
  local inhome="${HOME}/.ssh/fuchsia_ed25519"

  local have_in_tree=false
  local have_in_home=false
  test -f "$intree" && have_in_tree=true
  test -f "$inhome" && have_in_home=true

  if $have_in_tree && $have_in_home && ! cmp --silent "$intree" "$inhome"; then
    mismatched_keys
    exit $_ERROR_MISMATCHED_KEYS
  elif $have_in_tree && ! $have_in_home; then
    echo "Copying existing SSH credentials from //.ssh/pkey to ~/.ssh/fuchsia_ed25519"
    copy_ssh_keys "$intree" "$inhome" \
      "${FUCHSIA_ROOT}/.ssh/authorized_keys" \
      "${HOME}/.ssh/fuchsia_authorized_keys"
  elif ! $have_in_tree && $have_in_home; then
    echo "Copying existing SSH credentials from ~/.ssh/fuchsia_ed25519 to //.ssh/pkey"
    copy_ssh_keys "$inhome" "$intree" \
      "${HOME}/.ssh/fuchsia_authorized_keys" \
      "${FUCHSIA_ROOT}/.ssh/authorized_keys"
  fi
}

# $INFRA_RECIPES is a variable set by infra bots. This keeps the infra recipes
# running inside its per-build directory without seeking keys in a user's home
# directory. See fxr/415297.
if [[ "${INFRA_RECIPES}" == "1" ]]; then
  readonly SSH_DIR="${FUCHSIA_ROOT}/.ssh"
  readonly keyfile="${SSH_DIR}/pkey"
  readonly authfile="${SSH_DIR}/authorized_keys"
else
  readonly SSH_DIR="${HOME}/.ssh"
  readonly keyfile="${SSH_DIR}/fuchsia_ed25519"
  readonly authfile="${SSH_DIR}/fuchsia_authorized_keys"
  normalize_local_keys
fi

if [[ ! -f "${keyfile}" ]]; then
  if $no_new_key; then
    {
      echo "ERROR: SSH key doesn't exist: ${keyfile}."
      echo "Run //tools/ssh-keys/gen-ssh-keys.sh to generate a new one."
    } 1>&2
    exit $_ERROR_NO_KEY
  fi
  # force subshell to limit scope of umask
  ( umask 077 && mkdir -p "${SSH_DIR}"; )
  ssh-keygen -N "" -t ed25519 -f "${keyfile}" -C "${description}"
  ssh-keygen -y -f "${keyfile}" >>"${authfile}"

  if [[ "${INFRA_RECIPES}" != "1" ]]; then
    # normalize again, so that the key created is kept in sync
    normalize_local_keys
  fi
fi

if [[ -f "${FUCHSIA_ROOT}/.fx-ssh-path" ]]; then
  { read old_keyfile && read old_authfile; } < "${FUCHSIA_ROOT}/.fx-ssh-path"
  if [[ "${old_keyfile}" == "${keyfile}" && "${old_authfile}" == "${authfile}" ]]; then
    exit 0
  fi
fi

echo -e "${keyfile}\n${authfile}" >"${FUCHSIA_ROOT}/.fx-ssh-path"
