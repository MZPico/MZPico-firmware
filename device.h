#ifndef __DEVICE_H__
#define __DEVICE_H__

#include "pico/stdlib.h"
#include "fdc.h"
#include "qd.h"

extern FDC *fdc;
extern QD *qd;

void device_main(void);
void blink(uint8_t i);

#endif
