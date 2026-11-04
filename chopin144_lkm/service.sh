#!/system/bin/sh
MODDIR=${0%/*}
LOG=/data/adb/chopin144_experimental_v0.3.log

echo "[$(date)] Chopin144 V0.3 service started" >> "$LOG"
PANEL=/proc/device-tree/dsi@1400e000/panel2@1/compatible
if [ -r "$PANEL" ]; then
    COMPAT="$(tr -d '\000' < "$PANEL")"
    echo "panel=$COMPAT" >> "$LOG"
    case "$COMPAT" in
        *k10a_42_02_0b_dsc_vdo*) ;;
        *)
            echo "Unsupported panel; module not loaded." >> "$LOG"
            exit 0
            ;;
    esac
fi

sleep 8
/system/bin/insmod "$MODDIR/chopin144_experimental.ko" >> "$LOG" 2>&1
RET=$?
echo "insmod_exit=$RET" >> "$LOG"
exit 0
