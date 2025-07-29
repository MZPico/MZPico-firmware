#!/bin/bash
set -e

MANAGER_VERSION=$(cat manager-version.txt)
MAMAGER_FILENAME="mzpico.mzf"
TOOL_URL="https://github.com/MZPico/MZPico-800-Manager/releases/download/${MANAGER_VERSION}/${MAMAGER_FILENAME}"
echo "Downloading MZPico Manager version ${MANAGER_VERSION}..."

curl -L -O "$TOOL_URL"

echo "Generating C header..."
python fw_gen.py  "${MANAGER_FILENAME}" > device_fw.h

