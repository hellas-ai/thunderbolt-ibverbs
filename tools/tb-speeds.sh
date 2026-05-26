#!/usr/bin/env zsh
setopt NULL_GLOB; for d in /sys/bus/thunderbolt/devices/?-?; do [ -f $d/tx_speed ] || continue; echo "$(basename $d) $(<$d/device_name) tx=$(<$d/tx_speed)x$(<$d/tx_lanes) rx=$(<$d/rx_speed)x$(<$d/rx_lanes)"; done
