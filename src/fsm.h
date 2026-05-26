#pragma once
#include "app_globals.h"
// Finite State Machine

void fsmTransition(SystemState newState);
void fsmProcessEvent(SystemEvent event);
void fsmHandleState();
const char *getStateName(SystemState state);
