#pragma once
#include <cstdint>
#define MAX_DEV_NAME_LENGTH 8
#define MAX_DEVICES 3
typedef struct { char name[MAX_DEV_NAME_LENGTH]; } DEV_ENTRY;
extern DEV_ENTRY devices[MAX_DEVICES];
extern uint8_t device_count;
