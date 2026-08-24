#!/bin/sh
# A harmless stand-in for Piper used by say_broker_test.sh.
trap 'exit 0' TERM INT
printf '%s\n' "$$" > "$SAY_TEST_PID_FILE"
while :; do sleep 1; done
