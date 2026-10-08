# gmSettings.c notes

## 1. the file

`~/Library/Application Support/GenBridge Monitor/settings.txt`, one key=value a line. Devices are stored by
UID, not name or position, so the same interface is found again whatever else is plugged in. Unknown keys
are skipped, so an older build reads a newer file.
