# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

case "$(uname -s)" in
  Linux)
    readonly HOST_OS="linux"
    ;;
  Darwin)
    readonly HOST_OS="mac"
    ;;
  *)
    echo >&2 "Unknown operating system: $(uname -s)."
    exit 1
    ;;
esac

case "$(uname -m)" in
  x86_64)
    readonly HOST_CPU="x64"
    ;;
  aarch64)
    readonly HOST_CPU="arm64"
    ;;
  arm64)
    # TODO(fxbug.dev/97767): Stop redirecting mac-arm64 to mac-x64 binaries once prebuilt
    # arm64 binaries are available and included in mac-arm64 checkouts.
    if [[ "$HOST_OS" == "mac" ]]; then
      readonly HOST_CPU="x64"
    else
      readonly HOST_CPU="arm64"
    fi
    ;;
  *)
    echo >&2 "Unknown architecture: $(uname -m)."
    exit 1
    ;;
esac

readonly HOST_PLATFORM="${HOST_OS}-${HOST_CPU}"

readonly PREBUILT_3P_DIR="${FUCHSIA_DIR}/prebuilt/third_party"
readonly PREBUILT_TOOLS_DIR="${FUCHSIA_DIR}/prebuilt/tools"

readonly PREBUILT_AEMU_DIR="${PREBUILT_3P_DIR}/android/aemu/release/${HOST_PLATFORM}"
readonly PREBUILT_BINUTILS_DIR="${PREBUILT_3P_DIR}/binutils-gdb/${HOST_PLATFORM}"
readonly PREBUILT_BUILDIFIER="${PREBUILT_3P_DIR}/buildifier/${HOST_PLATFORM}/buildifier"
readonly PREBUILT_BUILDOZER="${PREBUILT_3P_DIR}/buildozer/${HOST_PLATFORM}/buildozer"
readonly PREBUILT_CGPT_DIR="${PREBUILT_TOOLS_DIR}/cgpt/${HOST_PLATFORM}"
readonly PREBUILT_CLANG_DIR="${PREBUILT_3P_DIR}/clang/${HOST_PLATFORM}"
readonly PREBUILT_CMAKE_DIR="${PREBUILT_3P_DIR}/cmake/${HOST_PLATFORM}"
readonly PREBUILT_DART_DIR="${PREBUILT_3P_DIR}/dart/${HOST_PLATFORM}"
readonly PREBUILT_EDK2_DIR="${PREBUILT_3P_DIR}/edk2"
readonly PREBUILT_FUTILITY_DIR="${PREBUILT_TOOLS_DIR}/futility/${HOST_PLATFORM}"
readonly PREBUILT_GCC_DIR="${PREBUILT_3P_DIR}/gcc/${HOST_PLATFORM}"
readonly PREBUILT_GN="${PREBUILT_3P_DIR}/gn/${HOST_PLATFORM}/gn"
readonly PREBUILT_GO_DIR="${PREBUILT_3P_DIR}/go/${HOST_PLATFORM}"
readonly PREBUILT_GOMA_DIR="${PREBUILT_3P_DIR}/goma/${HOST_PLATFORM}"
readonly PREBUILT_GRPCWEBPROXY_DIR="${PREBUILT_3P_DIR}/grpcwebproxy/${HOST_PLATFORM}"
readonly PREBUILT_NINJA="${PREBUILT_3P_DIR}/ninja/${HOST_PLATFORM}/ninja"
readonly PREBUILT_PYTHON3_DIR="${PREBUILT_3P_DIR}/python3/${HOST_PLATFORM}"
readonly PREBUILT_QEMU_DIR="${PREBUILT_3P_DIR}/qemu/${HOST_PLATFORM}"
readonly PREBUILT_RECLIENT_DIR="${FUCHSIA_DIR}/prebuilt/proprietary/third_party/reclient/${HOST_PLATFORM}"
readonly PREBUILT_RUST_BINDGEN_DIR="${PREBUILT_3P_DIR}/rust_bindgen/${HOST_PLATFORM}"
readonly PREBUILT_RUST_CARGO_OUTDATED_DIR="${PREBUILT_3P_DIR}/rust_cargo_outdated/${HOST_PLATFORM}"
readonly PREBUILT_RUST_DIR="${PREBUILT_3P_DIR}/rust/${HOST_PLATFORM}"
readonly PREBUILT_VDL_DIR="${FUCHSIA_DIR}/prebuilt/vdl"

# Used by //scripts/hermetic-env for portable shebang lines.
PREBUILT_ALL_PATHS=
PREBUILT_ALL_PATHS+="${PREBUILT_AEMU_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_CLANG_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_CMAKE_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_DART_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_GO_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_GOMA_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_GRPCWEBPROXY_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_PYTHON3_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_QEMU_DIR}/bin:"
PREBUILT_ALL_PATHS+="${PREBUILT_RUST_DIR}/bin"
readonly PREBUILT_ALL_PATHS
