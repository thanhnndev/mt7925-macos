#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
# Wi-Fi root-cause matrix in one boot (docs/ROADMAP-AWDL.md §10.72). Run as root, Wi-Fi on:
#   sudo bash scripts/wifi-ab.sh            -> build/wifi-ab-<time>.txt
# A as booted | B AWDL + second OMAC off | C B + set_domain "00" (reconnect) | D B + country + power tables at join
# (reconnect) | E D + AWDL on. Every row: gateway ping, download, upload through en1, the core's retry counters.
cd "$(dirname "$0")/.."
CTL=build/itlctl; IF=en1; OUT=build/wifi-ab-$(date +%Y%m%d-%H%M%S).txt
stat() { ioreg -l -w0 | grep -o '"stat-log" = "[^"]*"' | grep -o -E 'up=[0-9]+s|tx=[0-9]+|txf=[0-9/]+|rty=[0-9/]+|bw=[0-9]+' | tr '\n' ' '; }
waitlink() {
  for i in $(seq 1 90); do
    GW=$(ipconfig getoption $IF router 2>/dev/null)
    [ -n "$GW" ] && ping -b $IF -c 1 -t 1 $GW >/dev/null 2>&1 && return 0
    sleep 1
  done; echo "  link did not come back in 90 s"; return 1
}
measure() {
  echo "== $1 ($(date +%T))"
  GW=$(ipconfig getoption $IF router)
  echo "  before: $(stat)"
  ping -b $IF -c 20 -i 0.3 $GW 2>&1 | tail -2 | sed 's/^/  /'
  curl -s --interface $IF -o /dev/null -m 25 -w '  down %{speed_download} B/s\n' 'https://speed.cloudflare.com/__down?bytes=25000000'
  dd if=/dev/zero bs=1m count=10 2>/dev/null | curl -s --interface $IF -o /dev/null -m 25 -w '  up %{speed_upload} B/s\n' \
    --data-binary @- https://speed.cloudflare.com/__up
  echo "  after:  $(stat)"
  ioreg -l -w0 | grep -o '"m3-log" = "[^"]*"' | tr ';' '\n' | grep -E 'recover #|link lost|regulatory|clc |txpower|own mac' | tail -4 | sed 's/^/  log: /'
}
{
  waitlink || exit 1
  measure "A as booted"
  $CTL itl_awdloff=1; sleep 8
  measure "B awdl off"
  $CTL itl_ccworld=1 itl_lvl=6; sleep 5; waitlink; sleep 5
  measure "C awdl off, domain 00"
  $CTL itl_ccworld=0 itl_nopwr=0 itl_lvl=6; sleep 5; waitlink; sleep 5
  measure "D awdl off, country + power tables at join"
  $CTL itl_awdloff=0; sleep 20
  measure "E D + awdl on"
} 2>&1 | tee $OUT
echo "saved $OUT"
