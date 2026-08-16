#!/bin/bash

# Script pour capturer les logs BLF/SUBSCRIBE avec adb logcat
# Usage: ./debug_blf_logs.sh

echo "==== Clearing previous logs ===="
adb logcat -c

echo ""
echo "==== Capturing BLF/SUBSCRIBE logs (Press Ctrl+C to stop) ===="
echo ""
echo "Watching for:"
echo "  - subscribePresence calls (Kotlin/Android)"
echo "  - nativeSubscribePresence (JNI)"
echo "  - SIP SUBSCRIBE messages"
echo "  - on_buddy_state callbacks"
echo "  - presence_updated events"
echo ""

adb logcat -v threadtime 2>&1 | grep -E "(subscribePresence|nativeSubscribePresence|SIP MSG.*SUBSCRIBE|on_buddy_state|presence_updated|SUBSCRIBE.*buddyId|pjsua_buddy_add|nativeUnsubscribe)"
