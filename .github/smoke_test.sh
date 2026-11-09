#!/bin/bash
# Installs the APK on a running emulator, boots the bundled monitor through the
# app, drives it with commands and checks the guest output in logcat.
set -euo pipefail
APK=$(ls dist/*.apk | head -1)
adb install -r "$APK"
adb logcat -c
adb shell am start -W -n com.bxiidn.winddroid/.MainActivity --ez autorun true \
    --es input "help;info;selftest;bench;uptime;poweroff"
for i in $(seq 1 90); do
    if adb logcat -d -s WindDroid:I | grep -q "STOPPED"; then break; fi
    sleep 2
done
adb logcat -d -s WindDroid:I | tee guest.log
adb logcat -d | grep -E "FATAL EXCEPTION|libwinddroid|AndroidRuntime" > crash.log || true
cat crash.log
fail=0
for want in "WindDroid Monitor" "MIDR 000f0510" "selftest: all 6 checks passed" "sha256(abc) = ba7816bf" "speed:" "Powering off." "STOPPED status=2"; do
    if grep -qF "$want" guest.log; then echo "ok: $want"; else echo "MISSING: $want"; fail=1; fi
done
if grep -q "FATAL EXCEPTION" crash.log; then echo "app crashed"; fail=1; fi
exit $fail
