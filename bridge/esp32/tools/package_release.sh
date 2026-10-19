#!/usr/bin/env bash
#
# Packages built firmware for a release (bridge spec 15). Run after
# tools/build.sh has built every chip. Writes two folders:
#
#   build/release/   the files for the GitHub release
#       dspi-bridge-<version>-<chip>.bin          everything, flashed at 0x0,
#                                                 for a first installation
#       dspi-bridge-<version>-<chip>-update.bin   the app alone, for "Update
#                                                 firmware" on the settings page
#       SHA256SUMS
#   build/site/      the installer page for GitHub Pages: installer/index.html,
#                    an ESP Web Tools manifest and the full images
#
# The version is PROJECT_VER in CMakeLists.txt, the one the firmware reports.
#
# Usage:
#   tools/package_release.sh

set -euo pipefail

BRIDGE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="$(cd "$BRIDGE_DIR/../.." && pwd)"
IDF_VERSION="$(tr -d '[:space:]' < "$BRIDGE_DIR/idf_version.txt")"
CHIPS=(esp32 esp32s3 esp32c3 esp32c6)

version=$(sed -n -E 's/^set\(PROJECT_VER "([^"]+)"\)$/\1/p' "$BRIDGE_DIR/CMakeLists.txt")
if [[ -z "${version}" ]]; then
    echo "Could not read PROJECT_VER from CMakeLists.txt" >&2
    exit 1
fi

for chip in "${CHIPS[@]}"; do
    if [[ ! -f "$BRIDGE_DIR/build/${chip}/dspi_bridge.bin" ]]; then
        echo "No build for ${chip}; run tools/build.sh first" >&2
        exit 1
    fi
done

release="$BRIDGE_DIR/build/release"
site="$BRIDGE_DIR/build/site"
rm -rf "${release}" "${site}"
mkdir -p "${release}" "${site}"

# Merge each chip's bootloader, partition table and app into one image that
# starts at address 0, with the gaps filled, as ESP Web Tools and esptool
# write it. esptool comes from Espressif's image, like the build.
merge_all=""
for chip in "${CHIPS[@]}"; do
    merge_all+="(cd build/${chip} && python -m esptool --chip ${chip} merge-bin \
        -o ../release/dspi-bridge-${version}-${chip}.bin @flash_args > /dev/null) && "
done
docker run --rm \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "${REPO_DIR}:/repo" \
    --workdir /repo/bridge/esp32 \
    "espressif/idf:${IDF_VERSION}" \
    bash -c "${merge_all} true"

for chip in "${CHIPS[@]}"; do
    cp "$BRIDGE_DIR/build/${chip}/dspi_bridge.bin" \
        "${release}/dspi-bridge-${version}-${chip}-update.bin"
done
(cd "${release}" && shasum -a 256 -- *.bin > SHA256SUMS)

# The installer site: the page, the full images, and the manifest that tells
# ESP Web Tools which image suits the chip it finds.
cp -R "$BRIDGE_DIR/installer" "${site}/installer"
cp "${release}"/dspi-bridge-"${version}"-esp32*.bin "${site}/installer/"
rm "${site}"/installer/*-update.bin
cat > "${site}/installer/manifest.json" <<JSON
{
  "name": "DSPi Link bridge",
  "version": "${version}",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {"chipFamily": "ESP32", "parts": [{"path": "dspi-bridge-${version}-esp32.bin", "offset": 0}]},
    {"chipFamily": "ESP32-S3", "parts": [{"path": "dspi-bridge-${version}-esp32s3.bin", "offset": 0}]},
    {"chipFamily": "ESP32-C3", "parts": [{"path": "dspi-bridge-${version}-esp32c3.bin", "offset": 0}]},
    {"chipFamily": "ESP32-C6", "parts": [{"path": "dspi-bridge-${version}-esp32c6.bin", "offset": 0}]}
  ]
}
JSON
# GitHub Pages serves the repository's site root; send it to the installer.
cat > "${site}/index.html" <<'HTML'
<!doctype html>
<meta charset="utf-8" />
<meta http-equiv="refresh" content="0; url=installer/" />
<title>DSPi Link bridge</title>
<a href="installer/">Install the DSPi Link bridge</a>
HTML

echo "Release ${version}:"
ls -l "${release}"
echo "Installer site: ${site}"
