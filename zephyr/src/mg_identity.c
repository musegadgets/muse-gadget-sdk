/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mg_identity.h"

#include <stdio.h>

static char name[32];
static char node_id[24];
static char mac[18];
static char device_id[32];

void mg_identity_init(const char *name_prefix, const uint8_t a[6])
{
	snprintf(name, sizeof(name), "%s%02X%02X%02X", name_prefix, a[3], a[4], a[5]);
	snprintf(node_id, sizeof(node_id), "homelink-%02x%02x%02x", a[3], a[4], a[5]);
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4],
		 a[5]);
	snprintf(device_id, sizeof(device_id), "hatch-link:%s", mac);
}

const char *mg_identity_name(void)
{
	return name;
}

const char *mg_identity_node_id(void)
{
	return node_id;
}

const char *mg_identity_mac(void)
{
	return mac;
}

const char *mg_identity_device_id(void)
{
	return device_id;
}
