#!/usr/bin/env bash
# Build and/or flash Sticky MicroNotes for Seeed reTerminal Sticky.
#
# Usage:
#   ./scripts/flash_sticky.sh [PORT] [mode]
#
# PORT  default: /dev/cu.wchusbserial5C850496161
# mode  app   — firmware.bin at 0x10000 only (default, safe everyday)
#       full  — bootloader + partitions + app (first install onto a foreign image)
#       build — pio build, then app flash
#       erase — erase entire flash (does not reflash; last resort)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${1:-/dev/cu.wchusbserial5C850496161}"
MODE="${2:-app}"
ENV="reterminal_sticky"
BUILD=".pio/build/${ENV}"
BAUD="${FLASH_BAUD:-460800}"

die() { echo "error: $*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

esptool_cmd() {
  if have esptool.py; then
    echo esptool.py
  elif python3 -c "import esptool" >/dev/null 2>&1; then
    echo python3 -m esptool
  elif have python && python -c "import esptool" >/dev/null 2>&1; then
    echo python -m esptool
  else
    die "esptool not found. Install with: python3 -m pip install esptool"
  fi
}

need_bins() {
  [[ -f "${BUILD}/bootloader.bin" && -f "${BUILD}/partitions.bin" && -f "${BUILD}/firmware.bin" ]] \
    || die "missing ${BUILD}/*.bin — run: pio run -e ${ENV}"
}

echo "port  ${PORT}"
echo "mode  ${MODE}"
echo "root  ${ROOT}"

case "${MODE}" in
  build)
    have pio || die "PlatformIO not on PATH. Install: python3 -m pip install platformio"
    pio run -e "${ENV}"
    MODE=app
    ;;
esac

ESPTOOL="$(esptool_cmd)"

case "${MODE}" in
  erase)
    # shellcheck disable=SC2086
    ${ESPTOOL} --chip esp32s3 --port "${PORT}" erase_flash
    echo "flash erased. next: $0 ${PORT} full"
    ;;
  app)
    need_bins
    # shellcheck disable=SC2086
    ${ESPTOOL} --chip esp32s3 --port "${PORT}" --baud "${BAUD}" \
      --before default_reset --after hard_reset \
      write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB \
      0x10000 "${BUILD}/firmware.bin"
    ;;
  full)
    need_bins
    # shellcheck disable=SC2086
    ${ESPTOOL} --chip esp32s3 --port "${PORT}" --baud "${BAUD}" \
      --before default_reset --after hard_reset \
      write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB \
      0x0     "${BUILD}/bootloader.bin" \
      0x8000  "${BUILD}/partitions.bin" \
      0x10000 "${BUILD}/firmware.bin"
    ;;
  *)
    die "unknown mode '${MODE}' (use full|app|build|erase)"
    ;;
esac

echo
echo "monitor with either:"
echo "  idf.py -p ${PORT} monitor"
echo "  pio device monitor -e ${ENV} --port ${PORT} -b 115200"
