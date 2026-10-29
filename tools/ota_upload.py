"""Uploads firmware to a running Telltale over WiFi (POST /ota, see src/ota.h).

    python tools/ota_upload.py <firmware.bin> <host>

The password comes from the TELLTALE_OTA_PASS environment variable (= the device
password, under Settings on the screen). Used by `pio run -e lcd7-ota -t upload`.
"""

import os
import sys
import urllib.error
import urllib.request


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image, host = sys.argv[1], sys.argv[2]
    password = os.environ.get("TELLTALE_OTA_PASS")
    if not password:
        sys.exit("Set TELLTALE_OTA_PASS (the device password, under Settings on the screen)")

    with open(image, "rb") as f:
        data = f.read()
    print(f"Uploading {len(data)} bytes to http://{host}/ota ...")
    req = urllib.request.Request(
        f"http://{host}/ota",
        data=data,
        method="POST",
        headers={"X-OTA-Password": password, "Content-Type": "application/octet-stream"},
    )
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            print(resp.read().decode().strip())
    except urllib.error.HTTPError as e:
        sys.exit(f"Device refused the update: {e.code} {e.read().decode().strip()}")
    except OSError as e:
        sys.exit(f"Upload failed: {e}")


if __name__ == "__main__":
    main()
