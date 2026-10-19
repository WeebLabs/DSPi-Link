#!/usr/bin/env bash
#
# Builds the bridge firmware in Espressif's official Docker image, using the
# ESP-IDF version pinned in idf_version.txt, so no local ESP-IDF install is
# needed. CI runs this same script.
#
# Usage:
#   tools/build.sh                 build all four chips
#   tools/build.sh esp32s3         build one chip
#
# Each chip builds into build/<chip>/. The script fails if an image is larger
# than the 1.5 MB budget of bridge spec 14.1.

set -euo pipefail

BRIDGE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="$(cd "$BRIDGE_DIR/../.." && pwd)"
IDF_VERSION="$(tr -d '[:space:]' < "$BRIDGE_DIR/idf_version.txt")"
ALL_TARGETS=(esp32 esp32s3 esp32c3 esp32c6)
MAX_IMAGE_BYTES=$((1536 * 1024))

if [[ $# -eq 0 ]]; then
    targets=("${ALL_TARGETS[@]}")
else
    targets=("$@")
fi

for target in "${targets[@]}"; do
    if [[ ! " ${ALL_TARGETS[*]} " =~ " ${target} " ]]; then
        echo "Unknown chip '${target}'. Choose from: ${ALL_TARGETS[*]}" >&2
        exit 2
    fi
done

for target in "${targets[@]}"; do
    echo "=== Building for ${target} with ESP-IDF ${IDF_VERSION}"

    # ESP-IDF fills a chip's sdkconfig from sdkconfig.defaults only for keys
    # it does not have yet, so a changed default would be ignored by an
    # existing build. Start that chip's sdkconfig again whenever a defaults
    # file is newer than it.
    sdkconfig="${BRIDGE_DIR}/build/${target}/sdkconfig"
    if [[ -f "${sdkconfig}" ]] &&
        [[ -n "$(find "${BRIDGE_DIR}" -maxdepth 1 -name 'sdkconfig.defaults*' -newer "${sdkconfig}")" ]]; then
        echo "=== ${target}: sdkconfig.defaults changed; regenerating the configuration"
        rm "${sdkconfig}"
    fi

    # The whole repository is mounted because later components read
    # policy/, fixtures/ and js/ from it. Running as the caller's user keeps
    # build files owned by them; HOME points somewhere that user can write.
    # ESP-IDF then warns that the user id has no name, which is harmless.
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        --env HOME=/tmp \
        --volume "${REPO_DIR}:/repo" \
        --workdir /repo/bridge/esp32 \
        "espressif/idf:${IDF_VERSION}" \
        idf.py -B "build/${target}" -DIDF_TARGET="${target}" \
            -DSDKCONFIG="build/${target}/sdkconfig" build

    image="${BRIDGE_DIR}/build/${target}/dspi_bridge.bin"
    size=$(wc -c < "${image}" | tr -d ' ')
    echo "=== ${target}: ${size} bytes of ${MAX_IMAGE_BYTES} allowed"
    if (( size > MAX_IMAGE_BYTES )); then
        echo "Image for ${target} exceeds the 1.5 MB budget (bridge spec 14.1)" >&2
        exit 1
    fi
done
