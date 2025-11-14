#ifndef __DEVICE_HPP__
#define __DEVICE_HPP__

#include "pico/stdlib.h"
#include "fdc.hpp"
#include "qd.hpp"

extern FDCDevice *fdc;
extern QDDevice *qd;

void device_main(void);
void blink(uint8_t i);

#endif
