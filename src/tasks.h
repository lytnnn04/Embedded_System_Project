#pragma once
// FreeRTOS task functions

void taskFSM(void *parameter);
void taskWiFiMonitor(void *parameter);
void taskDisplay(void *parameter);
void taskEmail(void *parameter);
void taskFactoryReset(void *parameter);
