#!/bin/bash
# The timing report of the last build, run from an ASCII directory: trce of ISE 14.7 fails to write
# its report when the path of the .ncd has non-ASCII characters ("Загрузки" in the user's tree),
# although the analysis itself is fine. Copies the files to /tmp, runs trce and the second run of
# the Mini / Mini G (the mode of the core), copies the .twr files back and prints the summary.
#   ./timing_ascii.sh minig
set -e
board=${1:-minig}
here=$(cd "$(dirname "$0")" && pwd)
d=$(mktemp -d /tmp/kg-timing.XXXXXX)
trap 'rm -rf "$d"' EXIT INT TERM
cp "$here/karabas_$board.ncd" "$here/karabas_$board.pcf" "$here/karabas_$board.ucf" "$here/pcf_core.py" "$d/"
cp "$here/karabas_${board}_map.mrp" "$d/" 2>/dev/null || true

bash -c "cd '$d' && source /opt/Xilinx/14.7/ISE_DS/settings64.sh > /dev/null && \
  trce -intstyle ise -v 3 -s 2 -n 3 -fastpaths 'karabas_$board.ncd' -o '$board.twr' \
       'karabas_$board.pcf' -ucf 'karabas_$board.ucf'" > "$d/trce.log" 2>&1 || { tail -5 "$d/trce.log"; exit 1; }
cp "$d/$board.twr" "$here/karabas_$board.twr"

if [ "$board" = mini ] || [ "$board" = minig ]; then
  ( cd "$d" && python3 pcf_core.py "karabas_$board.pcf" "${board}_core.pcf" ) > "$d/userc.log" 2>&1 || true
  bash -c "cd '$d' && source /opt/Xilinx/14.7/ISE_DS/settings64.sh > /dev/null && \
    trce -intstyle ise -v 3 -s 2 -n 3 -fastpaths 'karabas_$board.ncd' -o '${board}_core.twr' \
         '${board}_core.pcf'" > "$d/core.log" 2>&1 || true
  cp "$d/${board}_core.twr" "$here/karabas_${board}_core.twr" 2>/dev/null || true
fi

grep -E "Number of (Slice LUTs|occupied Slices|RAMB16BWERs|RAMB8BWERs):" "$here/karabas_${board}_map.mrp" 2>/dev/null || true
grep -E "constraints? (was|were) met|constraints? not met|Timing Score" "$here/karabas_$board.twr" 2>/dev/null | sort -u || true
if [ -f "$here/karabas_${board}_core.twr" ]; then
  echo "mode of the core (-coremode):"
  grep -E "constraints? (was|were) met|constraints? not met|Timing errors" "$here/karabas_${board}_core.twr" | sort -u || true
fi
