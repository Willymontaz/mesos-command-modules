#! /bin/sh

set -e -x

mkdir -p build
cd build

cmake ..
make clang-format

updated_files_count=$(git diff --name-only | wc -l)

if [ -n "$TRAVIS" ]; then
  if [ "$updated_files_count" -ne "0" ]; then
    echo "[FAILED] Please run 'make clang-format' and commit the changes."
    exit 1
  fi
fi

make -j "$(nproc)"
TEST_RESOURCES_PATH=../tests/scripts/ make check
