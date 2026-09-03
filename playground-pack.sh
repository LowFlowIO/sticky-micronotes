#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
OUT="${2:-../playground-sticky-micronotes/firmware/1.0.0}"
BUILD="${ROOT}/.pio/build/reterminal_sticky"
mkdir -p "${OUT}"
python3 -m esptool --chip esp32s3 merge_bin \
  -o "${OUT}/sticky-micronotes-1.0.0-reterminal-sticky.bin" \
  --flash_mode dio --flash_freq 40m --flash_size 32MB \
  0x0     "${BUILD}/bootloader.bin" \
  0x8000  "${BUILD}/partitions.bin" \
  0x10000 "${BUILD}/firmware.bin"

BIN="${OUT}/sticky-micronotes-1.0.0-reterminal-sticky.bin"
SIZE=$(wc -c < "${BIN}" | tr -d ' ')
SHA=$(shasum -a 256 "${BIN}" | awk '{print $1}')
MD5=$(md5 -q "${BIN}" 2>/dev/null || md5sum "${BIN}" | awk '{print $1}')
cat > "${OUT}/manifest.json" << EOF
{
  "name": "Sticky MicroNotes",
  "version": "1.0.0",
  "flashSize": "32MB",
  "flashMode": "dio",
  "flashFreq": "40m",
  "baudRate": 460800,
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        {
          "path": "sticky-micronotes-1.0.0-reterminal-sticky.bin",
          "offset": 0,
          "size": ${SIZE},
          "sha256": "${SHA}",
          "md5": "${MD5}"
        }
      ]
    }
  ]
}
EOF
echo "wrote ${OUT}"
ls -l "${OUT}"
