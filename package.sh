#!/usr/bin/env bash

set -e

if [[ -z $ANDROID_NDK_HOME ]]; then
    echo "error: ANDROID_NDK_HOME variable is required to build this project"
    exit 1
fi

CONAN_PROFILE="$1"
if [[ -z "$1" ]]; then
    CONAN_PROFILE=default
fi

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PACKAGE_DIR="build/package"

cd ${SCRIPT_DIR}

mkdir -p ${PACKAGE_DIR}/madbfs/

echo "building madbfs and madbfs-server using build profile: ${CONAN_PROFILE}"

# build client
./build.sh "$CONAN_PROFILE"

cp build/Release/madbfs/madbfs ${PACKAGE_DIR}/madbfs/
strip ${PACKAGE_DIR}/madbfs/madbfs

# build server
./madbfs-server/build_all.sh "$CONAN_PROFILE"

cp madbfs-server/build/android-all-release/* ${PACKAGE_DIR}/madbfs/

# package
cd ${PACKAGE_DIR}
tar -czvf madbfs.tar.gz madbfs/

echo "---------------------------[ build complete ]----------------------------"
echo "package is built: ${PACKAGE_DIR}/madbfs.tar.gz"
