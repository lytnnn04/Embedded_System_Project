#pragma once
// LVGL bridge and UI update helpers.
// Bridge functions (updateScreenFromState, process_password_attempt, etc.)
// are declared in lvgl/ui_bridge.h with extern "C" linkage.
// Only non-bridge helpers are declared here.

void displayMessage(const char *line1, const char *line2);
void updateDisplay();
