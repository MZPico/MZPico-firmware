#ifndef _QD_H_
#define _QD_H_

#include "mz_devices.h"

#define QD_PORTS 4

#define QD_NO_DISC 0
#define QD_DISC_READY 1
#define QD_HEAD_HOME 2
#define QD_ERR 8
#define READBUFSIZE 512UL


typedef struct {
  MZDevice iface;
  ReadPortMapping read[QD_PORTS];
  WritePortMapping write[QD_PORTS];

  FIL fileQDimage;
  int emuQD_status;
  //registry SIO
  unsigned char WReg_A[8];
  unsigned char WReg_B[8];
  unsigned char RReg_A[3];
  unsigned char RReg_B[3];
  //adresy SIO registru
  unsigned char A_adr;
  unsigned char B_adr;

  unsigned char readbuf[READBUFSIZE];
  unsigned short readindex;
  int (*set_drive_content)(void *v_self, char *file_path);
} QD;

QD *qd_new(uint8_t base_port);
int qd_set_drive_content(void *v_self, char *file_path);

#endif
