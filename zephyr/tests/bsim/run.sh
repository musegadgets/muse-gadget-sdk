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

# Runs one BabbleSim scenario: the gadget image and the test central on a
# simulated 2.4 GHz channel. Exit status 0 means both devices passed.
#
#   run.sh GADGET_EXE CENTRAL_EXE ptt|offline|setup|secure [SIM_SECONDS]
#
# Needs BSIM_OUT_PATH (BabbleSim built with `make everything`). The images
# come from `west build -b nrf54l15bsim/nrf54l15/cpuapp`, see ../../AGENTS.md.
set -u

gadget=$(realpath "$1")
central=$(realpath "$2")
scenario=$3
seconds=${4:-30}

case $scenario in
ptt) gadget_test=gadget_ptt ;;
offline) gadget_test=gadget_offline ;;
setup) gadget_test=gadget_setup ;;
secure) gadget_test=gadget_pair ;;
*)
	echo "unknown scenario $scenario" >&2
	exit 2
	;;
esac

sim_id="mg_${scenario}_$$"
cd "${BSIM_OUT_PATH}/bin" || exit 2

"$gadget" -s="$sim_id" -d=0 -testid="$gadget_test" -rs=23 > "/tmp/${sim_id}_gadget.log" 2>&1 &
g=$!
"$central" -s="$sim_id" -d=1 -testid="central_${scenario}" -rs=42 > "/tmp/${sim_id}_central.log" 2>&1 &
c=$!
./bs_2G4_phy_v1 -s="$sim_id" -D=2 -sim_length="${seconds}e6" > "/tmp/${sim_id}_phy.log" 2>&1 &
p=$!

wait $g
g_rc=$?
wait $c
c_rc=$?
wait $p

echo "---- gadget ($gadget_test), exit $g_rc"
cat "/tmp/${sim_id}_gadget.log"
echo "---- central (central_${scenario}), exit $c_rc"
cat "/tmp/${sim_id}_central.log"

if [ $g_rc -eq 0 ] && [ $c_rc -eq 0 ]; then
	echo "PASS: $scenario"
	exit 0
fi
echo "FAIL: $scenario"
exit 1
