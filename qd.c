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

#include "ff.h"
#include "qd.h"

static FIL fileQDimage;

/* static */ volatile int emuQD_status;

//registry SIO
static volatile unsigned char WReg_A[8];
static volatile unsigned char WReg_B[8];
static volatile unsigned char RReg_A[3];
static volatile unsigned char RReg_B[3];
//adresy SIO registru
static volatile unsigned char A_adr;
static volatile unsigned char B_adr;

#define READBUFSIZE	512UL
static unsigned char readbuf[READBUFSIZE];
static unsigned short readindex;

void qd_read1(unsigned char *sync1, UINT *s1) {
	//if (emuQD_status&QD_ERR) {
	//	*sync1=ROM_code[readindex++];
	//} else {	
		if (readindex&(READBUFSIZE-1)) {
			*sync1=readbuf[readindex&(READBUFSIZE-1)];
		} else {
				f_lseek( &fileQDimage, readindex);
			f_read( &fileQDimage, readbuf, READBUFSIZE, s1 );
			*sync1=readbuf[0];
		}
		readindex++;
	//}
	*s1=1;
}

/* init function to set-up the correct image
 * for SD/MMC card file manager
 * Result: 0 = ERROR
 *         1 = OK
 */
int qd_init(void) {
  // v main.c mame uz otevreny disk a pripojeny file system
  // jsou to funkce disk_initialize(0); f_mount(0, &Fatfs);
  A_adr = 0;
  B_adr = 0;
	readindex=0;
  // soubor se otevre pomoci f_open(&file1, filename, FA_READ);
  // otestujeme otevreni, jinak se neprizname k disku
}

int qd_set_drive_content(char *file_path) {
 if (FR_OK == f_open( &fileQDimage, file_path, 1)) {
    // mame soubor
    emuQD_status = QD_DISC_READY | QD_HEAD_HOME;
    qd_init();
    return 1;
  } else {
    emuQD_status = QD_NO_DISC;
		return 0;
	}
}

/* main function called from request decoder in main.c
 * request type is on the bit15; read = 0b
 */
void qd_write(uint8_t addr_offset, uint8_t dt) {
  int icnt;
	
  /* Connection is like following:
   * offset = 0 - data register for A channel
   * offset = 1 - data register for B channel - unused
   * offset = 2 - command register for A channel
   * offset = 3 - command register for B channel
   */

    switch( addr_offset&0x03) {
    case 2 :
      WReg_A[A_adr] = dt;
      if (0 == A_adr) {
	A_adr = dt & 0x07;
	//changes in command reg
	if (((dt) & 0x38 ) == 0x18) { // channel reset
	  for (icnt = 0; icnt < 8; icnt++) WReg_A[icnt] = 0;
	}
      } else {
	// other regs
	if( 3 == A_adr) { // SIO Rx control
	  if (WReg_A[3] & 0x10) { // SIO hunt phase
	    RReg_A[0] |= 0x10;
	  }
	}
/* 	if( 5 == A_adr) { // SIO Tx control */
/*            // Write is not supported */
/*         } */

	// zero the address
	A_adr = 0;
      }


      break;

    case 3 :
      WReg_B[B_adr] = dt;
      if (0 == B_adr) {
	B_adr = dt &0x07;
      } else {
         if (2 == B_adr) {
            RReg_B[2] = dt;
         }

	if( 5 == B_adr) { // SIO Tx control
	  if (!(WReg_B[5] & 0x80)) { // QD motor neaktivni - DTR je v '1'
	    //soubor na zacatek
             	if (!(emuQD_status&QD_ERR)) f_lseek( &fileQDimage, 0);
             readindex=0;
             emuQD_status |= QD_HEAD_HOME;
	  }
	}
	// zero the address
	B_adr = 0;
      }
      break;

    case 0 : //no emulation of WRITE, we need just READ
      break;

    case 1 : //nothing
      break;

    }

}

uint8_t qd_read(uint8_t addr_offset) {
  int icnt;
  unsigned char sync1, sync2;
  UINT s1;
  unsigned char dt;

    switch(addr_offset & 0x03) {
    case 0:
	emuQD_status &= ~QD_HEAD_HOME;
//	f_read( &fileQDimage, &io_data, 1, &s1 );
	qd_read1(&dt, &s1);
    break;

    case 2:
      // tohle vse je presunuto z READ_F6
      // Hunt phase
      if ((WReg_A[3]&0x11) == 0x11) {
	RReg_A[0] |= 0x10;
	// opsano ze Zdenkova emulatoru, ale proc je to jen 8 kroku, nevim...
	qd_read1(&sync1, &s1);
	for(icnt = 0; icnt <8; icnt++) {
     qd_read1(&sync2, &s1);
	   if ((sync1 == WReg_A[6]) && (sync2 == WReg_A[7])) {
	     RReg_A[0] &= 0xEF; // inverze huntphase bitu a konec
	     break;
	   }
	   sync1 = sync2;
	}
      }
      // disk detected and write protection
      if ( QD_DISC_READY == (QD_DISC_READY & emuQD_status)) {
        RReg_A[0] |= 0x08; // mame disk
      } else {
        RReg_A[0] &= 0xF7; // nemame disk
      }
      RReg_A[0] &= 0xDF; // write protect
      RReg_A[0] |= 0x05; // Rx char available & Tx buffer empty

      dt = RReg_A[A_adr&0x03];
//      DBGPRINTF(DBGINF, "emu_QD: R2: pntr = %d\n", QDA_pointer);
      // zero the address
      A_adr = 0;
      break;

    case 3:
      // set if "QD head is at home"
      if (QD_HEAD_HOME == (QD_HEAD_HOME & emuQD_status)) {
        RReg_B[0] = 0;
      } else {
        RReg_B[0] = 8;
      }
      if (B_adr == 0) {
        dt = 0xff;
      } else {
        dt = RReg_B[B_adr&0x03];
      }
//      DBGPRINTF(DBGINF, "emu_QD: R3: adr_ %02X = %02X\n", B_adr, *io_data);

      // zero the address
      B_adr = 0;
      break;

    case 1: //nothing
      dt = 0xff;
      break;
    }

    return dt;
}

