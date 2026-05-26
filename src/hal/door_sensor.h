#pragma once
// Door sensor (MC38) functions and task

void doorSensorInit();
bool isDoorOpen();
void startDoorAlarm();
void stopDoorAlarm();
void beepAlarmContinuous();
void taskDoorSensor(void *parameter);
