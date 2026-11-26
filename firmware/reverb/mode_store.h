// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
#pragma once
int mode_store_load();    // saved mode or -1 (call once at start-up, before the engine starts)
void mode_store_poll();   // core1: commit when the engine signals a silent window
