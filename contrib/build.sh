#!/bin/bash

set -e

elapsed() {
  start=$1
  stop=$2
  diff=$(($stop-$start))
  min=$((10#$diff / 60))
  sec=$((10#$diff % 60))
  printf "%d:%02d" "$min" "$sec";
}

GCC=()
CLANG=()
TARGETS=()
MACHINES=()

while [[ $# -gt 0 ]]; do
  FLAG="$1"
  shift 1
  case "$FLAG" in
    "--no-gcc")
      NO_GCC=1
      ;;
    "--no-clang")
      NO_CLANG=1
      ;;
    "--no-deps")
      NO_DEPS=1
      ;;
    "--no-rust")
      NO_RUST=1
      ;;
    "--targets")
      IFS=',' read -r -a TARGETS <<< "$1"
      shift 1
      ;;
    "--machines")
      IFS=',' read -r -a MACHINES <<< "$1"
      shift 1
      ;;
    "--gcc-versions")
      IFS=',' read -r -a GCC <<< "$1"
      shift 1
      ;;
    "--clang-versions")
      IFS=',' read -r -a CLANG <<< "$1"
      shift 1
      ;;
    *)
      echo "Unknown flag: $FLAG" >&2
      exit 1
      ;;
  esac
done

FD_REPO_DIR=$(realpath $(dirname $(realpath "$0"))/..)

TEST_TARGETS=( unit-test fuzz-test integration-test )
BUILD_TARGETS=( all )
EXTRA_TARGETS=( asm ppp seccomp-policies )

BINARY_TARGETS=( fdctl fddev )
NONBINARY_TARGETS=( ${BUILD_TARGETS[@]} ${EXTRA_TARGETS[@]} ${TEST_TARGETS[@]} )

declare -A CUSTOM_TARGETS=()

CUSTOM_TARGETS+=( ["linux_gcc_minimal"]="${NONBINARY_TARGETS[@]}" )
CUSTOM_TARGETS+=( ["linux_gcc_noarch64"]="${NONBINARY_TARGETS[@]}" )
CUSTOM_TARGETS+=( ["linux_gcc_noarch128"]="${NONBINARY_TARGETS[@]}" )

CUSTOM_TARGETS+=( ["macos_clang_m1"]="${NONBINARY_TARGETS[@]}" )
CUSTOM_TARGETS+=( ["linux_clang_minimal"]="${NONBINARY_TARGETS[@]}" )
CUSTOM_TARGETS+=( ["linux_clang_noarch64"]="${NONBINARY_TARGETS[@]}" )
CUSTOM_TARGETS+=( ["linux_clang_noarch128"]="${NONBINARY_TARGETS[@]}" )
CUSTOM_TARGETS+=( ["freebsd_clang_noarch128"]="${NONBINARY_TARGETS[@]}" )

FAIL=0

if [[ $NO_GCC -ne 1 ]]; then
  if [[ ${#GCC[@]} -eq 0 ]]; then
    for gcc in $(ls /opt/gcc); do
      GCC+=( $gcc )
    done
  fi
else
  GCC=()
fi

if [[ $NO_CLANG -ne 1 ]]; then
  if [[ ${#CLANG[@]} -eq 0 ]]; then
    for clang in $(ls /opt/clang); do
      CLANG+=( $clang )
    done
  fi
else
  CLANG=()
fi

if [[ ${#MACHINES[@]} -eq 0 ]]; then
  for machine in $(ls $FD_REPO_DIR/config/machine); do
    MACHINES+=( $machine )
  done
fi

echo "Starting Build Matrix..."
echo "========================"
echo "Machines: [ ${MACHINES[@]} ]"
echo "Clang: [ ${CLANG[@]} ]"
echo "Gcc: [ ${GCC[@]} ]"
echo "Targets: [ ${TARGETS[@]} ]"
echo

echo "Setting up build environment..."
cd $FD_REPO_DIR
if [[ $NO_RUST -ne 1 ]]; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y > /dev/null 2>&1
fi
make -j distclean > /dev/null 2>&1

set +e

if [[ $NO_GCC -ne 1 ]]; then
  START=$(date +%s)
  echo "Starting gcc builds..."
  for compiler in "${GCC[@]}"; do
    if [[ ! -f /opt/gcc/$compiler/activate ]]; then
      echo "Environment activate script not found at /opt/gcc/$compiler... exiting."
      exit 2
    fi
    source /opt/gcc/$compiler/activate
    if [[ $NO_DEPS -ne 1 ]]; then
      ./deps.sh nuke
      FD_AUTO_INSTALL_PACKAGES=1 ./deps.sh +dev fetch check install > /dev/null 2>&1
    fi
    for machine in "${MACHINES[@]}"; do
      MACHINE="${machine%.mk}"
      if [[ "$MACHINE" != *"clang"* ]]; then
        if [[ ${#TARGETS[@]} -eq 0 ]]; then
          if [[ -n "${CUSTOM_TARGETS[${MACHINE}]}" ]]; then
            IFS=' ' read -r -a TARGETS <<< "${CUSTOM_TARGETS[${MACHINE}]}"
          else
            TARGETS+=( "${TEST_TARGETS[@]}"   )
            TARGETS+=( "${BUILD_TARGETS[@]}"  )
            TARGETS+=( "${EXTRA_TARGETS[@]}"  )
            TARGETS+=( "${BINARY_TARGETS[@]}" )
          fi
        fi
        echo "Starting builds for $MACHINE with $compiler..."
        FAILED=()
        start=$(date +%s)
        for target in "${TARGETS[@]}"; do
          MACHINE=${MACHINE} CC=gcc make -j $target
          if [[ $? -ne 0 ]]; then
            FAILED+=( $target )
            FAIL=1
          fi
        done
        stop=$(date +%s)
        printf "Done... Elapsed Time: "
        elapsed $start $stop
        echo
        if [[ ${#FAILED[@]} -gt 0 ]]; then
          echo "Failed Targets: ${FAILED[@]}"
        fi
        make -j distclean > /dev/null 2>&1
        echo
      fi
    done
  done
  STOP=$(date +%s)
  printf "Done with gcc builds... Total Elapsed Time: "
  elapsed $START $STOP
  echo
fi

if [[ $NO_CLANG -ne 1 ]]; then
  START=$(date +%s)
  echo "Starting clang builds..."
  for compiler in "${CLANG[@]}"; do
    if [[ ! -f /opt/clang/$compiler/activate ]]; then
      echo "Environment activate script not found at /opt/clang/$compiler... exiting."
      exit 2
    fi
    source /opt/clang/$compiler/activate
    if [[ $NO_DEPS -ne 1 ]]; then
      ./deps.sh nuke
      FD_AUTO_INSTALL_PACKAGES=1 ./deps.sh +dev fetch check install > /dev/null 2>&1
    fi
    for machine in "${MACHINES[@]}"; do
      MACHINE="${machine%.mk}"
      if [[ "$MACHINE" != *"gcc"* ]]; then
        if [[ ${#TARGETS[@]} -eq 0 ]]; then
          if [[ -n "${CUSTOM_TARGETS[${MACHINE}]}" ]]; then
            IFS=' ' read -r -a TARGETS <<< "${CUSTOM_TARGETS[${MACHINE}]}"
          else
            TARGETS+=( "${TEST_TARGETS[@]}"   )
            TARGETS+=( "${BUILD_TARGETS[@]}"  )
            TARGETS+=( "${EXTRA_TARGETS[@]}"  )
            TARGETS+=( "${BINARY_TARGETS[@]}" )
          fi
        fi
        echo "Starting builds for $MACHINE with $compiler..."
        FAILED=()
        start=$(date +%s)
        for target in "${TARGETS[@]}"; do
          MACHINE=${MACHINE} CC=clang make -j $target
          if [[ $? -ne 0 ]]; then
            FAILED+=( $target )
            FAIL=1
          fi
        done
        stop=$(date +%s)
        printf "Done... Elapsed Time: "
        elapsed $start $stop
        echo
        if [[ ${#FAILED[@]} -gt 0 ]]; then
          echo "Failed Targets: ${FAILED[@]}"
        fi
        make -j distclean > /dev/null 2>&1
        echo
      fi
    done
  done
  STOP=$(date +%s)
  printf "Done with clang builds... Total Elapsed Time: "
  elapsed $START $STOP
  echo
fi

exit $FAIL
