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

#ifndef MG_BUTTON_H_
#define MG_BUTTON_H_

#include <stdbool.h>

void mg_button_init(void);
/* The button is down (read directly; at power-up, before input events run). */
bool mg_button_held_at_boot(void);
/* Feeds a press or release as if it came from the button (tests). */
void mg_button_inject(bool pressed);

#endif
