# Frozen manifest for the SeedSigner ESP32-P4 firmware.
#
# Starts from the esp32 port default, then freezes the stdlib modules the
# SeedSigner Python app imports as top-level names. These are official
# micropython-lib (python-stdlib) packages; freezing them into the firmware
# means the deploy harness no longer has to vendor them to /lib.
include("$(PORT_DIR)/boards/manifest.py")

require("logging")
require("hmac")
