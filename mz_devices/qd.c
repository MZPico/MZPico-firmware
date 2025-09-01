/*
 *    Copyright (c) 2010 by Vaclav Peroutka <vaclavpe@seznam.cz>
 *    Copyright (c) 2012 by Bohumil Novacek <http://dzi.n.cz/8bit/>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    MZ-800 Unicard is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with MZ-800 Unicard; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * emu_QD.c
 *
 * This file is emulation port of Quick Disk MZ-1F11
 *
 * It is use during boot of Sharp MZ-800 to start SD/MMC card file manager
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include "ff.h"
#include "qd.h"

void qd_read1(QD *self, unsigned char *sync1, UINT *s1) {
	//if (emuQD_status&QD_ERR) {
	//	*sync1=ROM_code[readindex++];
	//} else {	
		if (self->readindex&(READBUFSIZE-1)) {
			*sync1=self->readbuf[self->readindex&(READBUFSIZE-1)];
		} else {
				f_lseek( &self->fileQDimage, self->readindex);
			f_read( &self->fileQDimage, self->readbuf, READBUFSIZE, s1 );
			*sync1=self->readbuf[0];
		}
		self->readindex++;
	//}
	*s1=1;
}

/* init function to set-up the correct image
 * for SD/MMC card file manager
 * Result: 0 = ERROR
 *         1 = OK
 */
int qd_init(void *v_self) {
  QD *self = (QD *)v_self;

  self->A_adr = 0;
  self->B_adr = 0;
  self->readindex=0;
}

int qd_set_drive_content(void *v_self, char *file_path) {
  QD *self = (QD *)v_self;
 if (FR_OK == f_open( &self->fileQDimage, file_path, 1)) {
    // mame soubor
    self->emuQD_status = QD_DISC_READY | QD_HEAD_HOME;
    qd_init(self);
    return 1;
  } else {
    self->emuQD_status = QD_NO_DISC;
		return 0;
	}
}

/* main function called from request decoder in main.c
 * request type is on the bit15; read = 0b
 */
int qd_write(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  QD *self = (QD *)v_self;
  int icnt;

  /* Connection is like following:
   * offset = 0 - data register for A channel
   * offset = 1 - data register for B channel - unused
   * offset = 2 - command register for A channel
   * offset = 3 - command register for B channel
   */

    switch (port & 0x03) {
    case 2 :
      self->WReg_A[self->A_adr] = dt;
      if (0 == self->A_adr) {
	self->A_adr = dt & 0x07;
	//changes in command reg
	if (((dt) & 0x38 ) == 0x18) { // channel reset
	  for (icnt = 0; icnt < 8; icnt++) self->WReg_A[icnt] = 0;
	}
      } else {
	// other regs
	if( 3 == self->A_adr) { // SIO Rx control
	  if (self->WReg_A[3] & 0x10) { // SIO hunt phase
	    self->RReg_A[0] |= 0x10;
	  }
	}
/* 	if( 5 == A_adr) { // SIO Tx control */
/*            // Write is not supported */
/*         } */

	// zero the address
	self->A_adr = 0;
      }


      break;

    case 3 :
      self->WReg_B[self->B_adr] = dt;
      if (0 == self->B_adr) {
	self->B_adr = dt &0x07;
      } else {
         if (2 == self->B_adr) {
            self->RReg_B[2] = dt;
         }

	if( 5 == self->B_adr) { // SIO Tx control
	  if (!(self->WReg_B[5] & 0x80)) { // QD motor neaktivni - DTR je v '1'
	    //soubor na zacatek
             	if (!(self->emuQD_status&QD_ERR)) f_lseek( &self->fileQDimage, 0);
             self->readindex=0;
             self->emuQD_status |= QD_HEAD_HOME;
	  }
	}
	// zero the address
	self->B_adr = 0;
      }
      break;

    case 0 : //no emulation of WRITE, we need just READ
      break;

    case 1 : //nothing
      break;

    }

}

int qd_read(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  QD *self = (QD *)v_self;
  int icnt;
  unsigned char sync1, sync2;
  UINT s1;

    switch(port & 0x03) {
    case 0:
	self->emuQD_status &= ~QD_HEAD_HOME;
//	f_read( &fileQDimage, &io_data, 1, &s1 );
	qd_read1(self, dt, &s1);
    break;

    case 2:
      // tohle vse je presunuto z READ_F6
      // Hunt phase
      if ((self->WReg_A[3]&0x11) == 0x11) {
	self->RReg_A[0] |= 0x10;
	// opsano ze Zdenkova emulatoru, ale proc je to jen 8 kroku, nevim...
	qd_read1(self, &sync1, &s1);
	for(icnt = 0; icnt <8; icnt++) {
     qd_read1(self, &sync2, &s1);
	   if ((sync1 == self->WReg_A[6]) && (sync2 == self->WReg_A[7])) {
	     self->RReg_A[0] &= 0xEF; // inverze huntphase bitu a konec
	     break;
	   }
	   sync1 = sync2;
	}
      }
      // disk detected and write protection
      if ( QD_DISC_READY == (QD_DISC_READY & self->emuQD_status)) {
        self->RReg_A[0] |= 0x08; // mame disk
      } else {
        self->RReg_A[0] &= 0xF7; // nemame disk
      }
      self->RReg_A[0] &= 0xDF; // write protect
      self->RReg_A[0] |= 0x05; // Rx char available & Tx buffer empty

      *dt = self->RReg_A[self->A_adr&0x03];
//      DBGPRINTF(DBGINF, "emu_QD: R2: pntr = %d\n", QDA_pointer);
      // zero the address
      self->A_adr = 0;
      break;

    case 3:
      // set if "QD head is at home"
      if (QD_HEAD_HOME == (QD_HEAD_HOME & self->emuQD_status)) {
        self->RReg_B[0] = 0;
      } else {
        self->RReg_B[0] = 8;
      }
      if (self->B_adr == 0) {
        *dt = 0xff;
      } else {
        *dt = self->RReg_B[self->B_adr&0x03];
      }
//      DBGPRINTF(DBGINF, "emu_QD: R3: adr_ %02X = %02X\n", B_adr, *io_data);

      // zero the address
      self->B_adr = 0;
      break;

    case 1: //nothing
      *dt = 0xff;
      break;
    }

    return 0;
}

QD *qd_new(uint8_t port_base) {
  QD *qd = calloc(1, sizeof *qd);
  for (int i = 0; i < QD_PORTS; i++) {
    qd->read[i].port = qd->write[i].port = port_base + i;
    qd->read[i].fn = qd_read;
    qd->write[i].fn = qd_write;
  }

  qd->iface.read = qd->read;
  qd->iface.read_port_count = QD_PORTS;

  qd->iface.write = qd->write;
  qd->iface.write_port_count = QD_PORTS;
  qd->iface.needs_exwait = 1;

  qd->iface.init = qd_init;
  qd->iface.is_interrupt = NULL;
  qd->set_drive_content = qd_set_drive_content;
  return qd;
}
