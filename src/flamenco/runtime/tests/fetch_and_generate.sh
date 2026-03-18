#!/bin/bash
set -euo pipefail

PROJECT_ROOT=../../../..

# Allow overriding proto version; default pinned
PROTO_VERSION="${PROTO_VERSION:-v5.3.0}"
FLATCC="${FLATCC:-${PROJECT_ROOT}/opt/bin/flatcc}"

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
FD_NANOPB_TAG=$(cat ${PROJECT_ROOT}/src/ballet/nanopb/nanopb_tag.txt)

# Create venv and install packages
python3.11 -m venv nanopb_venv
source nanopb_venv/bin/activate
pip install protobuf grpcio-tools

# Fetch nanopb
if [ ! -d nanopb ]; then
  git clone --depth=1 -q https://github.com/nanopb/nanopb.git
  cd nanopb
  git fetch --depth=1 -q origin $FD_NANOPB_TAG:refs/tags$FD_NANOPB_TAG
  git checkout -q $FD_NANOPB_TAG
  cd ..
else
  cd nanopb
  git fetch --depth=1 -q origin $FD_NANOPB_TAG:refs/tags$FD_NANOPB_TAG
  git checkout -q $FD_NANOPB_TAG
  cd ..
fi

# Use a local protosol checkout when provided.  This is useful when
# iterating on new harness schemas that have not landed upstream yet.
if [ -n "${PROTO_LOCAL_DIR:-}" ]; then
    rm -rf protosol
    ln -s "${PROTO_LOCAL_DIR}" protosol
else
    # Fetch protosol at specified tag/branch
    if [ ! -d protosol ]; then
        git clone --depth=1 --branch "$PROTO_VERSION" https://github.com/firedancer-io/protosol.git
    else
        cd protosol
        git fetch --tags
        git checkout "$PROTO_VERSION"
        cd ..
    fi
fi

rm -rf generated/*
./nanopb/generator/nanopb_generator.py -I ./protosol/proto -L "" -C ./protosol/proto/*.proto -D generated

# Generate flatbuffer headers
rm -rf flatbuffers/generated/*
$FLATCC --prefix=fd_ -a -I protosol/flatbuffers -r -o flatbuffers/generated/ protosol/flatbuffers/*.fbs
python3 fixup_flatbuffers.py
