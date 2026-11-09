#!/bin/bash
# Installs the APK on a running emulator, boots the bundled monitor through the
# app, drives it with commands and checks the guest output in logcat.
set -uo pipefail
export PATH="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}/platform-tools:$PATH"

annotate() {   # annotate LEVEL TITLE FILE: publish a file's tail as a workflow annotation
    local body
    body=$(tail -c 3500 "$3" 2>/dev/null | sed -e 's/%/%25/g' | sed -e ':a;N;$!ba;s/\n/%0A/g')
    echo "::$1 title=$2::${body:-<empty>}"
}

APK=$(ls dist/*.apk | head -1)
echo "adb: $(command -v adb)"
adb install -r "$APK" > install.log 2>&1 || { annotate error "adb install failed" install.log; exit 1; }
adb logcat -c
adb shell am start -W -n com.bxiidn.winddroid/.MainActivity --ez autorun true \
    --es input "'help;info;selftest;bench;uptime;poweroff'" > start.log 2>&1 || { annotate error "am start failed" start.log; exit 1; }
for i in $(seq 1 90); do
    if adb logcat -d -s WindDroid:I | grep -q "STOPPED"; then break; fi
    sleep 2
done
adb logcat -d -s WindDroid:I > guest.log
adb logcat -d | grep -E -A20 "FATAL EXCEPTION|F libc|F DEBUG" > crash.log || true
cat guest.log crash.log
annotate notice "guest output" guest.log
fail=0
for want in "WindDroid Monitor" "MIDR 000f0510" "selftest: all 6 checks passed" "sha256(abc) = ba7816bf" "speed:" "Powering off." "STOPPED status=2"; do
    if grep -qF "$want" guest.log; then echo "ok: $want"; else echo "::error::missing guest output: $want"; fail=1; fi
done
if [ -s crash.log ]; then annotate error "crash" crash.log; fail=1; fi
exit $fail
