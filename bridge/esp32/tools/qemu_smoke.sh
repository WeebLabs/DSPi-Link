#!/usr/bin/env bash
#
# Boots a built image twice in Espressif's QEMU, from the same flash, and
# checks that the first boot creates the settings and the second keeps the
# same hub id. This stands in for a real board where none is attached; QEMU
# has no Wi-Fi or UART peripherals worth testing, so it only proves the boot
# sequence and flash storage.
#
# Usage, after tools/build.sh <chip>:
#   tools/qemu_smoke.sh esp32s3
#
# Supported chips: esp32, esp32s3, esp32c3 (QEMU has no ESP32-C6 machine).

set -euo pipefail

BRIDGE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="$(cd "$BRIDGE_DIR/../.." && pwd)"
IDF_VERSION="$(tr -d '[:space:]' < "$BRIDGE_DIR/idf_version.txt")"
chip="${1:?usage: tools/qemu_smoke.sh <esp32|esp32s3|esp32c3>}"

case "${chip}" in
    esp32 | esp32s3) qemu=qemu-system-xtensa ;;
    esp32c3) qemu=qemu-system-riscv32 ;;
    *)
        echo "QEMU cannot run '${chip}'; choose esp32, esp32s3 or esp32c3" >&2
        exit 2
        ;;
esac

# Inside the container: merge the build into one 4 MB flash image, then boot
# it twice and keep only the bridge's own log lines. The original ESP32 is
# slow under emulation, hence the generous timeout.
boot_twice=$(cat <<SCRIPT
set -euo pipefail
cd build/${chip}
python -m esptool --chip ${chip} merge-bin --fill-flash-size 4MB -o /tmp/flash.bin @flash_args > /dev/null
for boot in 1 2; do
    echo "BOOT \${boot}"
    timeout 45 ${qemu} -nographic -machine ${chip} \
        -drive file=/tmp/flash.bin,if=mtd,format=raw 2>&1 | grep -E "I \([0-9]+\) (settings|bridge):" || true
done
SCRIPT
)

log=$(docker run --rm \
    --volume "${REPO_DIR}:/repo" \
    --workdir /repo/bridge/esp32 \
    "espressif/idf:${IDF_VERSION}" \
    bash -c "${boot_twice}" 2>/dev/null)
echo "${log}"

first_boot=$(sed -n '/^BOOT 1/,/^BOOT 2/p' <<< "${log}")
second_boot=$(sed -n '/^BOOT 2/,$p' <<< "${log}")
hub_id_pattern='[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}'
first_id=$(grep -oE "Hub \"[^\"]*\" \(${hub_id_pattern}\)" <<< "${first_boot}" | grep -oE "${hub_id_pattern}" || true)
second_id=$(grep -oE "Hub \"[^\"]*\" \(${hub_id_pattern}\)" <<< "${second_boot}" | grep -oE "${hub_id_pattern}" || true)

fail() {
    echo "FAILED (${chip}): $1" >&2
    exit 1
}
grep -q "First boot: saving factory settings" <<< "${first_boot}" || fail "first boot did not create settings"
grep -q "First boot" <<< "${second_boot}" && fail "second boot created settings again"
[[ -n "${first_id}" ]] || fail "first boot did not log a hub id"
[[ "${first_id}" == "${second_id}" ]] || fail "hub id changed between boots"
echo "PASSED (${chip}): settings created once, hub id ${first_id} kept"
