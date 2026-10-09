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

"""Gadget setup over Muse Link (protocols/README.md): provision_v2 with Wi-Fi
optional or absent (main/ble_server.c), the token-only commit with its
rollback (main/app.c), and a BLE-only gadget's boot recovery. The real
blocks run on the host with storage, BLE and tasks replaced by fakes."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_unpair_storage_contract import _function_body  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components/muse_gadget_ble"
PROTOCOLS = ROOT.parent / "protocols"
VECTORS = json.loads((PROTOCOLS / "test-vectors/mg-token-proof-v1.json").read_text(encoding="utf-8"))


def _between(source: str, start: str, end: str) -> str:
    i = source.index(start)
    return source[i:source.index(end, i)]


PROVISION_V2 = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "link_pairing.h"
#define mbedtls_platform_zeroize(p, n) memset((p), 0, (n))
#ifndef cJSON_IsBool
#define cJSON_IsBool(x) (cJSON_IsTrue(x) || cJSON_IsFalse(x))
#endif
#define pdPASS 1
static int task_fails;
static void *last_task;
#define xTaskCreate(fn, name, stack, arg, prio, handle) \
    (task_fails ? 0 : (last_task = (arg), (void)(fn), 1))
static char status[64];
static void ble_server_send_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void ble_server_send_pairing_status(const char *s, uint32_t g) { (void)g; ble_server_send_status(s); }
static void ble_server_disconnect_pairing_session(uint32_t g) { (void)g; }
static link_wifi_mode_t mode;
link_wifi_mode_t link_pairing_wifi_mode(void) { return mode; }
uint32_t link_pairing_mark_provisioning_active(void) { return 7; }
static void provision_task(void *arg) { (void)arg; }
@TYPES@
static void provision_v2(const char *json) {
    cJSON *root = cJSON_ParseWithLength(json, strlen(json));
    assert(root);
    bool decrypted = true;
    const char *act = "provision_v2";
    status[0] = 0;
    last_task = NULL;
    if (0) {
@BRANCH@
    }
    delete_command_json(root, decrypted);
}
static provision_args_t *taken(void) { return (provision_args_t *)last_task; }
static void release(void) {
    provision_args_t *a = taken();
    assert(a);
    free(a->ssid); free(a->password); free(a->access_token); free(a->refresh_token);
    free(a->username); free(a->ota_url); free(a->api_url); free(a->api_url_v2); free(a->noise_host);
    free(a);
    last_task = NULL;
}
#define TOKENS "\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\""
#define EXTRA ",\"username\":\"u\",\"api_url_v2\":\"https://api\",\"noise_host\":\"noise\",\"ota_url\":\"https://ota\""
static void refused(const char *json, const char *why) {
    provision_v2(json);
    if (strcmp(status, why) != 0 || taken()) {
        fprintf(stderr, "mode %d: %s -> '%s' (task %p), want %s\n", mode, json, status, last_task, why);
        abort();
    }
}
int main(void) {
    /* Required (every build without gadget support): as before. */
    mode = LINK_WIFI_REQUIRED;
    provision_v2("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS EXTRA "}");
    assert(!status[0] && taken() && !strcmp(taken()->ssid, "home") && !strcmp(taken()->ota_url, "https://ota"));
    assert(!strcmp(taken()->username, "u") && taken()->session_generation == 7);
    release();
    refused("{" TOKENS "}", "error_missing_credentials");
    refused("{\"ssid\":\"\",\"password\":\"\"," TOKENS "}", "error_missing_credentials");
    refused("{\"ssid\":\"home\"," TOKENS "}", "error_missing_credentials");

    /* Optional: no ssid and no password is token-only; an ssid is the Wi-Fi flow. */
    mode = LINK_WIFI_OPTIONAL;
    const char *token_only[] = {
        "{" TOKENS EXTRA "}",
        "{\"ssid\":\"\",\"password\":\"\"," TOKENS EXTRA "}",
        "{\"ssid\":\"\"," TOKENS EXTRA "}",
    };
    for (size_t i = 0; i < sizeof(token_only) / sizeof(token_only[0]); i++) {
        provision_v2(token_only[i]);
        provision_args_t *a = taken();
        assert(!status[0] && a && !a->ssid && !a->password && !strcmp(a->access_token, "a"));
        assert(!strcmp(a->refresh_token, "r") && !a->ota_url);          /* OTA ignored */
        assert(!strcmp(a->username, "u") && !strcmp(a->api_url_v2, "https://api")
               && !strcmp(a->noise_host, "noise"));                      /* kept for later Wi-Fi */
        release();
    }
    provision_v2("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS EXTRA "}");
    assert(!status[0] && taken() && !strcmp(taken()->ssid, "home") && taken()->ota_url);
    release();
    refused("{\"ssid\":\"home\"," TOKENS "}", "error_missing_credentials");
    refused("{\"password\":\"pw\"," TOKENS "}", "error_missing_credentials");
    refused("{\"ssid\":null," TOKENS "}", "error_missing_credentials");
    refused("{\"access_token\":\"a\",\"token_type\":\"device\"}", "error_missing_credentials");
    refused("{\"access_token\":\"a\",\"refresh_token\":\"r\"}", "error_missing_credentials");
    refused("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"user\"}", "error_missing_credentials");

    /* None: an ssid is refused, endpoints and username ignored. */
    mode = LINK_WIFI_NONE;
    provision_v2("{\"ssid\":\"\",\"password\":\"\"," TOKENS EXTRA "}");
    provision_args_t *a = taken();
    assert(!status[0] && a && !a->ssid && !a->ota_url && !a->username && !a->api_url_v2 && !a->noise_host);
    release();
    refused("{\"ssid\":\"home\",\"password\":\"pw\"," TOKENS "}", "error_wifi_unsupported");
    refused("{\"password\":\"pw\"," TOKENS "}", "error_wifi_unsupported");
    refused("{\"ssid\":\"home\"}", "error_missing_credentials");   /* the tokens come first */

    /* A provisioning task that can't start. */
    task_fails = 1;
    refused("{" TOKENS "}", "error_operation_in_progress");
    printf("PASS provision_v2\n");
    return 0;
}
"""

TOKEN_ONLY = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_pairing.h"
#include "mg_token_proof.h"
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define LED_STATE_AUTH_OK 0
typedef enum { CONFIG_KEY_LOOKUP_ERROR = -1, CONFIG_KEY_NOT_FOUND = 0, CONFIG_KEY_FOUND = 1 } config_key_lookup_t;

/* NVS: a small string store with one key that can be made to fail. */
static char keys[32][16], vals[32][128];
static const char *fail_key;
static int find(const char *k) {
    for (int i = 0; i < 32; i++) if (keys[i][0] && !strcmp(keys[i], k)) return i;
    return -1;
}
static bool config_set_str(const char *k, const char *v) {
    if (fail_key && !strcmp(fail_key, k)) return false;
    int i = find(k);
    for (int j = 0; i < 0 && j < 32; j++) if (!keys[j][0]) i = j;
    assert(i >= 0 && strlen(v) < sizeof(vals[0]));
    snprintf(keys[i], sizeof(keys[i]), "%s", k);
    snprintf(vals[i], sizeof(vals[i]), "%s", v);
    return true;
}
static bool config_erase_key(const char *k) {
    if (fail_key && !strcmp(fail_key, k)) return false;
    int i = find(k);
    if (i >= 0) keys[i][0] = 0;
    return true;
}
static const char *get(const char *k) { int i = find(k); return i < 0 ? NULL : vals[i]; }
static config_key_lookup_t config_key_lookup(const char *k) { return find(k) < 0 ? CONFIG_KEY_NOT_FOUND : CONFIG_KEY_FOUND; }
static bool config_clear_setup(void) { memset(keys, 0, sizeof(keys)); return true; }
static bool config_mark_setup_complete(void) { return config_set_str("setup_complete", "1"); }
static bool config_setup_complete(void) { return get("setup_complete") && !strcmp(get("setup_complete"), "1"); }
static bool config_mark_wifi_skipped(void) { return config_set_str("setup_wifi", "skipped"); }
static bool config_wifi_skipped(void) { return get("setup_wifi") && !strcmp(get("setup_wifi"), "skipped"); }
static bool config_is_provisioned(void) { return get("access_token") != NULL; }
static int count(void) { int n = 0; for (int i = 0; i < 32; i++) n += keys[i][0] != 0; return n; }

/* Link and the app around the commit. */
static link_wifi_mode_t mode;
static bool session_valid = true;
static int stops, setup_changes, fresh;
static char events[8][64];
static int nevents;
static void record(const char *what) {
    assert(nevents < 8);
    snprintf(events[nevents++], sizeof(events[0]), "%s%s", what, config_setup_complete() ? "+committed" : "");
}
link_wifi_mode_t link_pairing_wifi_mode(void) { return mode; }
bool link_pairing_mgcommands(void) { return true; }
bool link_pairing_commit_provisioning(uint32_t g, bool (*commit)(void)) { return g == 9 && session_valid && commit(); }
static const char *identity_node_id(void) { return "@NODE@"; }
static void setup_fail_for_session(const char *stage, const char *status, uint32_t g) {
    (void)stage; assert(g == 9);
    config_clear_setup();   /* as the app's: setup_wipe_to_clean() */
    char what[64];
    snprintf(what, sizeof(what), "fail:%s", status);
    record(what);
}
static bool require_provisioning_pairing_session(uint32_t g) {
    if (session_valid) return true;
    setup_fail_for_session("pairing", "auth_failed", g);
    return false;
}
static void auth_lock_take(void) {}
static void auth_lock_give(void) {}
static void note_access_token_fresh(void) { fresh++; }
static void store_pairing_username_unlocked(const char *u) {
    if (u && *u) config_set_str("username", u); else config_erase_key("username");
}
static void setup_stage_set(const char *s) { (void)s; }
static void ui_set_status(const char *s) { (void)s; }
static void led_status_set_state(int s) { (void)s; }
static void mg_glue_setup_changed(void) { setup_changes++; }
static void ble_server_send_pairing_status(const char *s, uint32_t g) { assert(g == 9); record(s); }
static void stop_setup_ble(const char *why) { (void)why; stops++; }

@COMMIT@

static void provision_token_only_with_gate_held(const char *access_token, const char *refresh_token,
                                                const char *username, const char *api_url,
                                                const char *api_url_v2, const char *noise_host,
                                                uint32_t session_generation) {
@TOKEN_ONLY@
}
static bool standalone_boot(void) {
@STANDALONE@
    return setup_complete;
}
static void reset(void) {
    config_clear_setup();
    fail_key = NULL;
    session_valid = true;
    stops = setup_changes = fresh = nevents = 0;
}
static void run(void) {
    provision_token_only_with_gate_held("@ACCESS@", "refresh-token", "user@example.com", "https://old",
                                        "https://api", "noise.example", 9);
}
int main(void) {
    /* Optional Wi-Fi: everything in one commit, auth_ok only after it. */
    mode = LINK_WIFI_OPTIONAL;
    reset();
    run();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
    assert(!strcmp(get("access_token"), "@ACCESS@") && !strcmp(get("refresh_token"), "refresh-token"));
    assert(!strcmp(get("mg_proof_k"), "@K@"));
    assert(config_wifi_skipped() && config_setup_complete() && !get("ssid"));
    assert(!strcmp(get("username"), "user@example.com") && !strcmp(get("api_url_v2"), "https://api")
           && !strcmp(get("noise_host"), "noise.example") && !strcmp(get("api_url"), "https://old"));
    assert(stops == 1 && setup_changes == 1 && fresh == 1);

    /* No Wi-Fi at all: no endpoints or username. */
    mode = LINK_WIFI_NONE;
    reset();
    run();
    assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed") && count() == 5);
    assert(!get("username") && !get("api_url_v2") && !get("noise_host") && !strcmp(get("mg_proof_k"), "@K@"));

    /* Any write failing: error_storage, nothing left, no auth_ok, advertising stays. */
    const char *failing[] = {"setup_wifi", "refresh_token", "access_token", "auth_token", "mg_proof_k",
                             "setup_complete", "api_url_v2", "username"};
    for (size_t i = 0; i < sizeof(failing) / sizeof(failing[0]); i++) {
        mode = LINK_WIFI_OPTIONAL;
        reset();
        fail_key = failing[i];
        run();
        if (!strcmp(failing[i], "username")) {
            /* The username is best effort, as in the Wi-Fi flow. */
            assert(nevents == 1 && !strcmp(events[0], "auth_ok+committed"));
            continue;
        }
        if (nevents != 1 || strcmp(events[0], "fail:error_storage") != 0 || count() != 0 || stops) {
            fprintf(stderr, "failing %s: %d events (%s), %d keys, %d stops\n", failing[i], nevents,
                    nevents ? events[0] : "", count(), stops);
            abort();
        }
    }

    /* The session went away first: a pairing failure, nothing stored. */
    reset();
    session_valid = false;
    run();
    assert(nevents == 1 && !strcmp(events[0], "fail:auth_failed") && count() == 0 && !stops);

    /* A BLE-only gadget's boot: set up means the marker and the key. */
    reset();
    config_set_str("setup_complete", "1");
    config_set_str("mg_proof_k", "@K@");
    config_set_str("access_token", "a");
    assert(standalone_boot() && count() == 3);
    reset();
    config_set_str("setup_complete", "1");       /* no key: set up by Wi-Fi firmware */
    config_set_str("access_token", "a");
    assert(!standalone_boot() && count() == 0);
    reset();
    config_set_str("setup_wifi", "skipped");     /* cut off midway */
    config_set_str("access_token", "a");
    assert(!standalone_boot() && count() == 0);
    reset();
    config_set_str("mg_proof_k", "@K@");
    assert(!standalone_boot() && count() == 0);
    reset();
    config_set_str("unrelated", "kept");          /* never set up */
    assert(!standalone_boot() && count() == 1);
    printf("PASS token-only commit\n");
    return 0;
}
"""


ADVERTISING = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ble_server.h"
#define TAG "ble"
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGI(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_sink(__VA_ARGS__)
#define ESP_LOGD(tag, ...) log_sink(__VA_ARGS__)
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
struct ble_hs_adv_fields {
    uint8_t flags, num_uuids128, uuids128_is_complete, mfg_data_len, name_len, name_is_complete;
    ble_uuid128_t *uuids128;
    const uint8_t *mfg_data, *name;
};
struct ble_gap_adv_params { uint8_t conn_mode, disc_mode; uint16_t itvl_min, itvl_max; };
struct ble_gap_event { int type; };
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
#define BLE_UUID_TYPE_128 128
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_GAP_ADV_ITVL_MS(ms) ((uint16_t)((ms) * 8 / 5))
#define BLE_HS_FOREVER INT32_MAX
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define ADV_TURN_MS 1500
#define MAX_COMPANIONS 2
static const ble_uuid128_t SVC_UUID = { { BLE_UUID_TYPE_128 }, { 0x1c } };   /* Link setup */
static const uint8_t MG_UUID[16] = { 0x5d };                                 /* a companion */
static char s_device_name[] = "MuseGadget-A7F5B4";
static bool s_started = true, s_shutting_down, s_synced, s_advertising_enabled, s_advertising_active;
static bool s_adv_turn, s_companion_advertising, s_adv_link, setup_done, companion_wants = true;
static const ble_companion_t *s_adv_other;
static ble_companion_t s_companions[MAX_COMPANIONS];
static int s_ncompanions;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int starts, stops, last_duration;
static uint8_t staged, last_uuid;
static int gap_event_cb(struct ble_gap_event *e, void *a) { (void)e; (void)a; return 0; }
static bool config_setup_complete(void) { return setup_done; }
static bool wants(void) { return companion_wants; }
static int ble_hs_util_ensure_addr(int random) { (void)random; return 0; }
static int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) { staged = f->uuids128->value[0]; return 0; }
static int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) { (void)f; return 0; }
static int ble_gap_adv_start(uint8_t own, const void *peer, int32_t duration, const struct ble_gap_adv_params *p,
                             ble_gap_event_fn *cb, void *arg) {
    (void)own; (void)peer; (void)p; (void)cb; (void)arg;
    assert(!s_advertising_active);   /* NimBLE refuses a second start */
    starts++; last_duration = duration; last_uuid = staged;
    return 0;
}
static int ble_gap_adv_stop(void) { stops++; return 0; }
void ble_server_disconnect_client(void) {}
static void rx_reset(void) {}
static void link_pairing_reset(void) {}
static bool s_plaintext_status_blocked;
static void start_advertising(void);
static void turn_ends(void) { s_advertising_active = false; s_adv_turn = !s_adv_turn; start_advertising(); }
@REGION@
@BEGIN@
@STOP@
@COMPANION@
int main(void) {
    /* A gadget: the musegadgets companion wants advertising from boot. */
    s_companions[s_ncompanions++] = (ble_companion_t){ .adv_uuid128 = MG_UUID, .wants_advertising = wants };
    /* Host sync comes first: the companion alone, with no end. */
    on_sync();
    assert(starts == 1 && last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    /* Then Link setup asks to be found: restarted, taking turns. */
    ble_server_begin_advertising();
    assert(stops == 1 && starts == 2 && last_duration == ADV_TURN_MS && s_adv_link && s_adv_other);
    turn_ends();
    assert(starts == 3 && last_uuid != (uint8_t)(s_adv_turn ? 0x1c : 0x5d) && last_duration == ADV_TURN_MS);
    turn_ends();
    assert(starts == 4 && last_uuid == (s_adv_turn ? 0x5d : 0x1c));
    /* Asking again changes nothing, and nothing restarts. */
    ble_server_begin_advertising();
    ble_server_set_companion_advertising(false);
    assert(stops == 1 && starts == 4);
    /* Setup completes: the companion alone again. */
    setup_done = true;
    ble_server_stop_advertising(false);
    assert(stops == 2 && starts == 5 && last_uuid == 0x5d && last_duration == BLE_HS_FOREVER);
    /* Begin, the other order: advertising starts at sync with both. */
    setup_done = false; s_advertising_active = false; s_advertising_enabled = false; s_synced = false;
    starts = stops = 0;
    ble_server_begin_advertising();
    assert(starts == 0);
    on_sync();
    assert(starts == 1 && last_duration == ADV_TURN_MS);
    /* A build without a companion: Link setup only, never restarted by asking again. */
    s_ncompanions = 0; s_advertising_active = false; s_advertising_enabled = false; starts = stops = 0;
    on_sync();
    assert(starts == 0);
    ble_server_begin_advertising();
    assert(starts == 1 && last_uuid == 0x1c && last_duration == BLE_HS_FOREVER);
    ble_server_begin_advertising();
    ble_server_set_companion_advertising(true);    /* Muse's phone setup: the same payload */
    assert(starts == 1 && stops == 0);
    printf("PASS advertising\n");
    return 0;
}
"""


class LinkGadgetSetupTest(unittest.TestCase):
    def setUp(self) -> None:
        self.cc = shlex.split(os.environ.get("CC", "cc"))
        if not self.cc or shutil.which(self.cc[0]) is None:
            self.skipTest("C compiler not available")

    def build_and_run(self, source: str, extra: list[Path]) -> str:
        with tempfile.TemporaryDirectory(prefix="link-gadget-") as directory:
            path = Path(directory) / "harness.c"
            path.write_text(source)
            binary = Path(directory) / "harness"
            r = subprocess.run(
                [*self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/link_fakes"), "-I", str(MG / "include"),
                 "-I", str(PROTOCOLS), str(path), *[str(p) for p in extra], "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout

    def test_advertising_follows_what_is_wanted(self) -> None:
        # Host sync may start the companion's advertising before Link setup
        # asks for its own: the running advertisement must be restarted.
        ble = (ROOT / "main/ble_server.c").read_text()
        source = (ADVERTISING
                  .replace("@REGION@", _between(ble, "// The companion whose UUID wants advertising now",
                                                "static void on_reset(int reason)"))
                  .replace("@BEGIN@", _between(ble, "void ble_server_begin_advertising(void) {",
                                               "void ble_server_disconnect_client(void)"))
                  .replace("@STOP@", _between(ble, "void ble_server_stop_advertising(bool disconnect_client) {",
                                              "void ble_server_set_companion(const ble_companion_t"))
                  .replace("@COMPANION@", _between(ble, "void ble_server_set_companion_advertising(bool enabled) {",
                                                   "bool ble_server_is_started(void)")))
        self.assertIn("PASS advertising", self.build_and_run(source, []))

    def test_provision_v2_wifi_modes(self) -> None:
        ble = (ROOT / "main/ble_server.c").read_text()
        branch = _between(ble, '    } else if (decrypted && strcmp(act, "provision_v2") == 0) {',
                          '    } else if (strcmp(act, "get_device_info") == 0) {')
        types = _between(ble, "typedef struct {\n    char *ssid;", "static void provision_task(")
        types += "static const char *optional_ota_url(cJSON *root) {" + _function_body(
            ble, "static const char *optional_ota_url(cJSON *root)") + "}\n"
        types += "static bool optional_ota_force(cJSON *root) {" + _function_body(
            ble, "static bool optional_ota_force(cJSON *root)") + "}\n"
        source = PROVISION_V2.replace("@TYPES@", types).replace("@BRANCH@", branch)
        out = self.build_and_run(source, [ROOT / "tests/link_fakes/cJSON.c"])
        self.assertIn("PASS provision_v2", out)

    def test_token_only_commit_and_rollback(self) -> None:
        app = (ROOT / "main/app.c").read_text()
        commit = _between(app, "#if CONFIG_MUSE_GADGET_BLE_AUDIO\n// The gadget's part of a provisioning commit",
                          "static void stop_setup_ble(")
        token_only = _function_body(app, "static void provision_token_only_with_gate_held(")
        standalone = _between(app, "    bool setup_complete = config_setup_complete();\n    bool have_key",
                              '    s_setup_stage = setup_complete ? "done" : "idle";\n    ui_set_ble("musegadgets");')
        basic = VECTORS["vectors"][0]
        source = (TOKEN_ONLY.replace("@COMMIT@", commit).replace("@TOKEN_ONLY@", token_only)
                  .replace("@STANDALONE@", standalone).replace("@NODE@", basic["node_id"])
                  .replace("@ACCESS@", basic["access_token"]).replace("@K@", basic["K"]))
        out = self.build_and_run(source, [ROOT / "tests/mg_token_proof_ref.c"])
        self.assertIn("PASS token-only commit", out)

    def test_auth_ok_follows_the_commit(self) -> None:
        # The order the harness checks, in the source too: commit, then auth_ok,
        # then Link setup advertising stops; no Wi-Fi status on the way.
        app = (ROOT / "main/app.c").read_text()
        body = _function_body(app, "static void provision_token_only_with_gate_held(")
        self.assertLess(body.index("link_pairing_commit_provisioning(session_generation, commit_setup)"),
                        body.index('ble_server_send_pairing_status("auth_ok", session_generation)'))
        self.assertLess(body.index('ble_server_send_pairing_status("auth_ok", session_generation)'),
                        body.index("stop_setup_ble("))
        for gone in ("wifi_mgr", '"wifi_', "connect_preferred_vm", "ota_"):
            self.assertNotIn(gone, body)


if __name__ == "__main__":
    unittest.main()
