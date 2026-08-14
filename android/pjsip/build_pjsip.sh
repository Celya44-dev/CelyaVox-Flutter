#!/usr/bin/env bash
set -euo pipefail

PJSIP_VERSION="2.17"
PJSIP_TARBALL="pjproject-${PJSIP_VERSION}.tar.gz"
PJSIP_URL="https://github.com/pjsip/pjproject/archive/refs/tags/${PJSIP_VERSION}.tar.gz"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${ROOT_DIR}/pjproject-${PJSIP_VERSION}"
CONFIG_SITE="${ROOT_DIR}/config_site.h"

# ABIs we build for
ABIS=("arm64-v8a")
API_LEVEL=24

if [[ -z "${ANDROID_NDK_ROOT:-}" && -z "${NDK_HOME:-}" ]]; then
  echo "ERROR: ANDROID_NDK_ROOT (or NDK_HOME) must be set" >&2
  exit 1
fi
NDK_PATH="${ANDROID_NDK_ROOT:-${NDK_HOME}}"

fetch_sources() {
  if [[ -d "${SRC_DIR}" ]]; then
    return
  fi
  echo "Downloading PJSIP ${PJSIP_VERSION}..."
  wget -O "${PJSIP_TARBALL}" "${PJSIP_URL}"
  tar -xzf "${PJSIP_TARBALL}"
  mv "pjproject-${PJSIP_VERSION}" "${SRC_DIR}"
  echo "Sources downloaded and extracted. Checking headers..."
  ls -la "${SRC_DIR}/pjlib/include/pjlib.h" || echo "pjlib.h not found in sources"
}

prepare_config() {
  cp "${CONFIG_SITE}" "${SRC_DIR}/pjlib/include/pj/config_site.h"
}

build_for_abi() {
  local abi="$1"
  echo "\n=== Building for ${abi} ==="
  pushd "${SRC_DIR}" >/dev/null
  make distclean >/dev/null 2>&1 || true

  export ANDROID_NDK="${NDK_PATH}"
  export TARGET_ABI="${abi}"
  export APP_PLATFORM="android-${API_LEVEL}"

  local host_triple=""
  case "${abi}" in
    arm64-v8a)
      host_triple="aarch64-unknown-linux-android"
      ;;
    armeabi-v7a)
      host_triple="arm-unknown-linux-androideabi"
      ;;
    *)
      host_triple=""
      ;;
  esac

  ./configure-android \
    --use-ndk-cflags \
    --with-ssl=no \
    --with-sdl=no \
    --with-openh264=no \
    --with-v4l2=no \
    --disable-video \
    --disable-gsm \
    --disable-l16 \
    --disable-speex \
    --disable-ilbc \
    --disable-opus

  # Skip building third_party to avoid codec issues
  sed -i 's/ third_party//' Makefile
  sed -i 's/pjnath\/build\/build/pjnath\/build/g' Makefile

  make dep
  make clean
  make

  local out_dir="${ROOT_DIR}/../app/src/main/jniLibs/${abi}"
  mkdir -p "${out_dir}"

    # Helper: copy first match from a preferred directory, with fallback find
    copy_norm() {
      local preferred_dir="$1"; shift
      local base_name="$1"; shift
      local so_pattern="$1"; shift
      local a_pattern="$1"; shift
      local so_dest="$1"; shift
      local a_dest="$1"; shift
      local found

      if [[ -n "$host_triple" && -d "$preferred_dir" ]]; then
        if [[ -f "$preferred_dir/lib${base_name}-${host_triple}.so" ]]; then
          cp -a "$preferred_dir/lib${base_name}-${host_triple}.so" "${out_dir}/${so_dest}"
          return
        fi
        if [[ -f "$preferred_dir/lib${base_name}-${host_triple}.a" ]]; then
          cp -a "$preferred_dir/lib${base_name}-${host_triple}.a" "${out_dir}/${a_dest}"
          return
        fi
      fi

      if [[ -d "$preferred_dir" ]]; then
        found=$(find "$preferred_dir" -name "$so_pattern" -type f -print -quit)
        if [[ -n "$found" ]]; then
          cp -a "$found" "${out_dir}/${so_dest}"
          return
        fi
        found=$(find "$preferred_dir" -name "$a_pattern" -type f -print -quit)
        if [[ -n "$found" ]]; then
          cp -a "$found" "${out_dir}/${a_dest}"
          return
        fi
      fi

      found=$(find . -name "$so_pattern" -type f -print -quit)
      if [[ -n "$found" ]]; then
        cp -a "$found" "${out_dir}/${so_dest}"
        return
      fi

      found=$(find . -name "$a_pattern" -type f -print -quit)
      if [[ -n "$found" ]]; then
        cp -a "$found" "${out_dir}/${a_dest}"
      fi
    }

    # Core libs (shared or static), normalized names expected by CMake
    copy_norm "pjlib/lib" "pj" "libpj*.so" "libpj*.a" "libpj.so" "libpj.a"
    copy_norm "pjlib-util/lib" "pjlib-util" "libpjlib-util*.so" "libpjlib-util*.a" "libpjlib-util.so" "libpjlib-util.a"
    copy_norm "pjnath/lib" "pjnath" "libpjnath*.so" "libpjnath*.a" "libpjnath.so" "libpjnath.a"
    copy_norm "pjmedia/lib" "pjmedia" "libpjmedia*.so" "libpjmedia*.a" "libpjmedia.so" "libpjmedia.a"
    copy_norm "pjmedia/lib" "pjmedia-codec" "libpjmedia-codec*.so" "libpjmedia-codec*.a" "libpjmedia-codec.so" "libpjmedia-codec.a"
    copy_norm "pjmedia/lib" "pjmedia-audiodev" "libpjmedia-audiodev*.so" "libpjmedia-audiodev*.a" "libpjmedia-audiodev.so" "libpjmedia-audiodev.a"
    copy_norm "pjsip/lib" "pjsip-core" "libpjsip-core*.so" "libpjsip-core*.a" "libpjsip-core.so" "libpjsip-core.a"
    copy_norm "pjsip/lib" "pjsip" "libpjsip*.so" "libpjsip*.a" "libpjsip.so" "libpjsip.a"
    copy_norm "pjsip/lib" "pjsip-simple" "libpjsip-simple*.so" "libpjsip-simple*.a" "libpjsip-simple.so" "libpjsip-simple.a"
    copy_norm "pjsip/lib" "pjsip-ua" "libpjsip-ua*.so" "libpjsip-ua*.a" "libpjsip-ua.so" "libpjsip-ua.a"
    copy_norm "pjsip/lib" "pjsua" "libpjsua*.so" "libpjsua*.a" "libpjsua.so" "libpjsua.a"
    copy_norm "pjsip/lib" "pjsua2" "libpjsua2*.so" "libpjsua2*.a" "libpjsua2.so" "libpjsua2.a"

    # Locate and copy pjsua2 (shared or static) since path varies per toolchain
    local pjsua2_lib
    # Try ABI-specific match first; otherwise grab any available libpjsua2.
    pjsua2_lib=$(find . \( -name "libpjsua2*${abi}*.so" -o -name "libpjsua2*${abi}*.a" \) -print -quit || true)
    if [[ -z "$pjsua2_lib" ]]; then
      pjsua2_lib=$(find . -name "libpjsua2*.so" -o -name "libpjsua2*.a" | head -n1 || true)
    fi
    if [[ -n "$pjsua2_lib" ]]; then
      # Normalize name so CMake expects libpjsua2.(so|a) without extra suffixes.
      if [[ "$pjsua2_lib" == *.so ]]; then
        cp -a "$pjsua2_lib" "${out_dir}/libpjsua2.so"
      else
        cp -a "$pjsua2_lib" "${out_dir}/libpjsua2.a"
      fi
    else
      echo "WARN: libpjsua2 (so/a) not found for ${abi}; SIP JNI may fail" >&2
    fi
  popd >/dev/null
}

main() {
  fetch_sources
  prepare_config
  for abi in "${ABIS[@]}"; do
    build_for_abi "${abi}"
  done
  echo "Build finished. Native libs are in android/app/src/main/jniLibs/"
}

main "$@"
