#!/bin/bash
##############################################################################
# Example command to build the android target.
##############################################################################
#
# This script shows how one can build a Caffe2 binary for the Android platform
# using android-cmake. A few notes:
#
# (1) This build also does a host build for protobuf. You will need autoconf
#     to carry out this. If autoconf is not possible, you will need to provide
#     a pre-built protoc binary that is the same version as the protobuf
#     version under third_party.
#     If you are building on Mac, you might need to install autotool and
#     libtool. The easiest way is via homebrew:
#         brew install automake
#         brew install libtool
# (2) You will need to have android ndk installed. The current script assumes
#     that you set ANDROID_NDK to the location of ndk.
# (3) The toolchain and the build target platform can be specified with the
#     cmake arguments below. For more details, check out android-cmake's doc.
# (4) By default this build on armeabi-v7a. You would want to put the various
#     builds in different directories, e.g. to build for x86, a good way is:
#         ANDROID_NDK=~/Android/Sdk/ndk-bundle/ BUILD_ROOT=$(pwd)/build_android_x86 ./scripts/build_android.sh -DANDROID_ABI=x86
# (5) You will need the libraries in build_android*/libs for each target abi
#     and the (target independent) header files, e.g. from
#     build/lib.*/torch/lib/include after you run ./setup.py build
#     (namely ATen, caffe2, c10, google) to build things on android.
#     You might also need Eigen headers.

set -e

CAFFE2_ROOT="$( cd "$(dirname "$0")"/.. ; pwd -P)"

if [ -z "$ANDROID_NDK" ]; then
  echo "ANDROID_NDK not set; please set it to the Android NDK directory"
  exit 1
fi

if [ ! -d "$ANDROID_NDK" ]; then
  echo "ANDROID_NDK not a directory; did you install it under $ANDROID_NDK?"
  exit 1
fi

echo "Bash: $(/bin/bash --version | head -1)"
echo "Caffe2 path: $CAFFE2_ROOT"
echo "Using Android NDK at $ANDROID_NDK"

# Build protobuf from third_party so we have a host protoc binary.
echo "Building protoc"
$CAFFE2_ROOT/scripts/build_host_protoc.sh

# Now, actually build the Android target.
BUILD_ROOT=${BUILD_ROOT:-"$CAFFE2_ROOT/build_android"}
mkdir -p $BUILD_ROOT
cd $BUILD_ROOT

CMAKE_ARGS=()

# If Ninja is installed, prefer it to Make
if [ -x "$(command -v ninja)" ]; then
  CMAKE_ARGS+=("-GNinja")
fi

# Use locally built protoc because we'll build libprotobuf for the
# target architecture and need an exact version match.
CMAKE_ARGS+=("-DCAFFE2_CUSTOM_PROTOC_EXECUTABLE=$CAFFE2_ROOT/build_host_protoc/bin/protoc")

# Use android-cmake to build Android project from CMake.
CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake")

# Don't build artifacts we don't need
CMAKE_ARGS+=("-DUSE_AVX=OFF")
CMAKE_ARGS+=("-DBUILD_TEST=OFF")
CMAKE_ARGS+=("-DBUILD_BINARY=OFF")
CMAKE_ARGS+=("-DBUILD_PYTHON=OFF")
CMAKE_ARGS+=("-DBUILD_SHARED_LIBS=OFF")
CMAKE_ARGS+=("-DANDROID_TOOLCHAIN=clang")
# Disable unused dependencies
CMAKE_ARGS+=("-DUSE_CUDA=OFF")
CMAKE_ARGS+=("-DUSE_GFLAGS=OFF")
CMAKE_ARGS+=("-DUSE_OPENCV=OFF")
CMAKE_ARGS+=("-DUSE_LMDB=OFF")
CMAKE_ARGS+=("-DUSE_LEVELDB=OFF")
CMAKE_ARGS+=("-DUSE_MPI=OFF")
CMAKE_ARGS+=("-DUSE_OPENMP=OFF")

# Only toggle if VERBOSE=1
if [ "${VERBOSE:-}" == '1' ]; then
  CMAKE_ARGS+=("-DCMAKE_VERBOSE_MAKEFILE=1")
fi

# Android specific flags
CMAKE_ARGS+=("-DANDROID_NDK=$ANDROID_NDK")
CMAKE_ARGS+=("-DANDROID_ABI=armeabi-v7a with NEON")
CMAKE_ARGS+=("-DANDROID_NATIVE_API_LEVEL=21")
CMAKE_ARGS+=("-DANDROID_CPP_FEATURES=rtti exceptions")

# Use-specified CMake arguments go last to allow overridding defaults
CMAKE_ARGS+=($@)

echo cmake "$CAFFE2_ROOT" \
    -DCMAKE_INSTALL_PREFIX=../install \
    -DCMAKE_BUILD_TYPE=Release \
    "${CMAKE_ARGS[@]}"

# Cross-platform parallel build
if [ -z "$MAX_JOBS" ]; then
  if [ "$(uname)" == 'Darwin' ]; then
    MAX_JOBS=$(sysctl -n hw.ncpu)
  else
    MAX_JOBS=$(nproc)
  fi
fi
echo cmake --build . -- "-j${MAX_JOBS}"
