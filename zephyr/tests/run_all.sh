#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Runs every host test: the native_sim unit tests (three configurations) and
# the BabbleSim scenarios. Linux only (x86-64 for BabbleSim, which needs
# 32-bit host builds: gcc-multilib). Needs ZEPHYR_BASE, and BSIM_OUT_PATH /
# BSIM_COMPONENTS_PATH for BabbleSim. Build output goes under $OUT.
#
#   tests/run_all.sh [unit|bsim|all]
set -u

here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
out=${OUT:-/tmp/mg-tests}
what=${1:-all}
board=${UNIT_BOARD:-native_sim/native/64}
fail=0

# west build, retried when a host tool dies without a real error message
# (compilers and Python crash now and then under QEMU user emulation on
# arm64 Macs, cc1 with "internal compiler error"; harmless elsewhere).
wbuild() {
	local log=$1
	shift
	for i in 1 2 3 4 5 6 7 8; do
		west build "$@" > "$log" 2>&1 && return 0
		grep -q "internal compiler error" "$log" ||
			! grep -qE ": (fatal )?error:|CMake Error at /work|warning: attempt to assign" "$log" ||
			return 1
		set -- $(printf '%s\n' "$@" | grep -vx -- -p)
	done
	return 1
}

run_unit() {
	local name=$1
	shift
	echo "== unit: $name"
	wbuild "$out/unit_$name.log" -p -b "$board" "$app/tests/unit" -d "$out/unit_$name" -- "$@" ||
		{ tail -30 "$out/unit_$name.log"; fail=1; return; }
	# Fresh simulated flash each run: a flash.bin left by another
	# configuration (RRAM-style vs NOR-style) would not be erased.
	"$out/unit_$name/zephyr/zephyr.exe" -flash_in_ram > "$out/unit_$name.run.log" 2>&1
	grep -E "^SUITE|PROJECT EXECUTION" "$out/unit_$name.run.log"
	grep -q "PROJECT EXECUTION SUCCESSFUL" "$out/unit_$name.run.log" || fail=1
}

bsim_build() {
	local dir=$1 src=$2
	shift 2
	wbuild "$out/$dir.log" -p -b nrf54l15bsim/nrf54l15/cpuapp "$src" -d "$out/$dir" -- "$@" ||
		{ tail -30 "$out/$dir.log"; fail=1; return 1; }
}

mkdir -p "$out"
if [ "$what" = unit ] || [ "$what" = all ]; then
	run_unit plain
	run_unit rram -DCONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=n
	run_unit slot1 -DCONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=n \
		-DCONFIG_MG_QUEUE_IN_SLOT1=y -DDTC_OVERLAY_FILE=slot1.overlay -DCONFIG_MG_HAPTICS=n
fi
if [ "$what" = bsim ] || [ "$what" = all ]; then
	echo "== bsim: building"
	bsim_build gadget "$app" -DEXTRA_CONF_FILE=tests/bsim/gadget.conf &&
		bsim_build central "$app/tests/bsim/central" &&
		for s in ptt offline setup; do
			g=$out/gadget/zephyr/zephyr.exe
			"$here/bsim/run.sh" "$g" "$out/central/zephyr/zephyr.exe" "$s" 40 \
				> "$out/bsim_$s.log" 2>&1 || fail=1
			grep -E "INFO: \(|Test end|ERROR" "$out/bsim_$s.log" | sed 's/^d_0[01]: //'
			tail -1 "$out/bsim_$s.log"
		done
fi
[ $fail -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED (logs in $out)"
exit $fail
