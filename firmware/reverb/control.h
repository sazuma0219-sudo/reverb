// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
#pragma once
void control_setup();
void control_loop();
void control_next_mode();
void control_set_override(int k, float v);
void control_clear_overrides();
float control_knob(int k);
float control_cv_volts(int i);
float control_override(int k);
