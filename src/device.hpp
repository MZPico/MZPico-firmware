#ifndef __DEVICE_HPP__
#define __DEVICE_HPP__

#include "pico/stdlib.h"
#include "fdc.hpp"
#include "qd.hpp"
#include "sn76489.hpp"

extern FDCDevice *fdc;
extern QDDevice *qd;
extern SN76489Device *sn76489;
extern volatile bool shutting_down;


void device_main(void);
void blink(uint8_t i);

#endif
