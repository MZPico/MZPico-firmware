/*
 *    Copyright (c) 2009 by Michal Hucik <http://www.ordoz.com>
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
 * Popis chovani radice WD279x: http://www.scav.ic.cz/sharp_mz-800/sharp_mz-800_8_FDC-WD2793.htm
 *
 * Popis struktury DSK souboru: http://www.kjthacker.f2s.com/docs/extdsk.html
 *
 */


#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "bus.h"
#include "fdc.h"

//#define DBGLEVEL	(DBGNON /*(DBGFAT | DBGERR | DBGWAR | DBGINF*/)
#define DBGLEVEL	(/*DBGNON | DBGFAT |*/ DBGERR /*| DBGWAR | DBGINF*/)

#define USE_DBG_PRN
//#include "debug.h"
#define DBGPRINTF(a,...)	{}

#define RETURN_FDC_ERR 1
#define RETURN_FDC_OK  0

// definice bool
#define true	1
#define false	0

uint8_t SUPPRESSED_DBGMSG = 0;

#define FAT_WRITE_SUPPORT 1

/*
 * FDC_GetTrackOffset() - podle tabulky stop v aktualnim DSK souboru spocita offset pro pozadovanou stopu.
 *
 * Prijma:
 *   - drive_id   [ 0 - 3 ], cislo mechaniky
 *   - track
 *   - side
 *
 * Vraci:
 *   offset v DSK souboru, nebo 0 v pripade chyby
 */
static int32_t FDC_GetTrackOffset (FDC *self, uint8_t drive_id, uint8_t track, uint8_t side ) {
   uint8_t i, buffer;
   unsigned int ff_readlen;
    uint32_t offset = 0;
    int32_t seek_offset;

    seek_offset = 0x34;   // Nastavime se na zacatek tabulky stop

    if ( FR_OK != f_lseek ( &(self->drive[ drive_id ].fh), seek_offset ) ) {
       return ( 0 ); // seek error
    }
    for ( i = 0; i < (( track * 2)  + side ); i++ ) {
       f_read( &(self->drive[ drive_id ].fh), &buffer, 1, &ff_readlen );
       if ( 1 != ff_readlen ) { return ( 0 ); } // read error

       if ( buffer == 0x00 ) return ( 0 ); // track not exist

       // BUGFIX: HD DSK files has wrong Sharp boot track info
       if ( i == 1 ) {
          if ( buffer == 0x25 ) {
             buffer = 0x11;       // zde vnutime bezny pocet stop
          };
       };

       offset += buffer * 0x100;
    };

    offset += 0x100;


    DBGPRINTF(DBGINF, "FDC_GetTrackOffset(): DRIVE: 0x%x, TRACK: 0x%, SIDE: 0x%x, track_offset: 0x%x\n", drive_id, track, offset);

    return ( offset );
}



/*
 * FDC_SeekToSector() - podle tabulky sektoru na prave nastavene stope
 * spocita a nastavi pozici v aktualnim DSK souboru na zacatek pozadovaneho sektoru
 * Spocita a nastavi:
 *      self->drive[ drive_id ].sector_size
 *      self->drive[ drive_id ].SECTOR
 *
 * Prijma:
 *   - drive_id   [ 0 - 3 ], cislo mechaniky
 *   - sector   pozadovany sektor
 *
 * Vraci:
 *   0 - vse je v poradku
 *   1 - v pripade chyby, nebo pokud sektor nebyl nalezen
 */
static uint8_t FDC_SeekToSector (FDC *self, uint8_t drive_id, uint8_t sector ) {
    uint8_t   i, sector_count;
    uint8_t   buffer [ 8 ];
    unsigned int ff_readlen;
    uint16_t   offset = 0;
    int32_t   seek_offset;

    self->drive[ drive_id ].sector_size = 0;

    seek_offset = self->drive[ drive_id ].track_offset + 0x15;
    if ( FR_OK != f_lseek ( &(self->drive[ drive_id ].fh), seek_offset ) ) {
       return ( 1 ); // seek error
    }

    f_read( &(self->drive[ drive_id ].fh), &sector_count, 1, &ff_readlen );
    if ( 1 != ff_readlen ) { return ( 1 ); } // read error

    seek_offset = self->drive[ drive_id ].track_offset + 0x18;
    if ( FR_OK != f_lseek ( &(self->drive[ drive_id ].fh), seek_offset ) ) {
       return ( 1 ); // seek error
    }

    for ( i = 0 ; i < sector_count; i++ ) {
       f_read( &(self->drive[ drive_id ].fh), buffer, 8, &ff_readlen );
       if ( 8 != ff_readlen ) { return ( 1 ); } // read error

       if ( sector == buffer [ 2 ] ) {
          self->drive[ drive_id ].sector_size = buffer [ 3 ] * 0x100;
          break;
       };
       offset += buffer [ 3 ] * 0x100;
    };

    if ( self->drive[ drive_id ].sector_size == 0 ) return ( 1 );   // sektor nebyl nalezen

    seek_offset = self->drive[ drive_id ].track_offset + offset + 0x100;
    if ( FR_OK == f_lseek ( &(self->drive[ drive_id ].fh), seek_offset ) ) {
       self->drive[ drive_id ].SECTOR = sector;

       DBGPRINTF(DBGINF, "FDC_SeekToSector(): DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, sector_offset: 0x%x\n",
    		   drive_id, self->drive[ drive_id ].TRACK, self->drive[ drive_id ].SIDE, sector, seek_offset);

       return ( 0 );

    } else {
       // seek error
       self->drive[ drive_id ].SECTOR = 0;
       self->drive[ drive_id ].sector_size = 0;
       return ( 1 );
    }
}

/*
 * FDC_setFDfromCFG() - podle obsahu konfiguracniho souboru /unicard/fd[0-3].cfg
 * otevre prislusny DSK soubor do zvolene mechaniky.
 * Pokud je jiz v mechanice nejaky DSK otevren, tak jej korektne zavre.
 * Pokud konfiguracni soubor neexistuje, nebo nelze otevrit, tak nastavi vsechny
 * hodnoty ve strukture aktualni mechaniky na nulu.
 *
 * Prijma: drive_id
 *
 * Vraci:
 *   -1 v pripade chyby
 *       0 pokud zadny disk neprimountoval
 *    1 pokud primountoval disk
 */

int fdc_set_drive_content(void *v_self, uint8_t drive_id, char *file_path) {
   FDC *self = (FDC *)v_self;
    FIL fh;
    unsigned int ff_readlen;

    if ( self->drive[drive_id].fh.obj.fs ) {
       if( FR_OK != f_sync (&(self->drive[drive_id].fh)) ) return ( -1 ); // sync error
       f_close( &(self->drive[drive_id].fh ));
    };
    memset ( &self->drive[ drive_id ], 0x00, sizeof ( self->drive[ drive_id ] ) ); //tohle bych asi radeji nedelal...
                                                                               //
    strcpy(self->drive [ drive_id ].path, file_path);

/*     while ( self->drive [ drive_id ].path [ strlen ( self->drive [ drive_id ].path ) - 1] < 0x20 ) { */
/*         self->drive [ drive_id ].path [ strlen ( self->drive [ drive_id ].path ) - 1] = '\0'; */
/*         if ( self->drive [ drive_id ].path [ 0 ] == '\0' ) return ( -1 ); // bad /path/filename in cfg file */
/*     }; */

/*     fname = strrchr ( self->drive [ drive_id ].path, '/' ); */
/*     strncpy ( self->drive [ drive_id ].filename, fname +1 , sizeof ( self->drive [ drive_id ].filename ) ); */
/*     if ( self->drive [ drive_id ].filename [ 0 ] == '\0' ) { */
/*         self->drive [ drive_id ].path [ 0 ] = '\0'; */
/*         return ( -1 );   // bad filename in cfg file */
/*     }; */
/*     fname [ 0 ] = '\0'; */

	DBGPRINTF(DBGINF, "FDC_setFDfromCFG(): New cfg: '%s' and file '%s', DRIVE: 0x%x\n",
			self->drive [ drive_id ].path, self->drive[drive_id].filename, drive_id);

    if ( FR_OK != f_open ( &(self->drive[drive_id].fh), self->drive[drive_id].path, FA_READ|FA_WRITE )) {

    	DBGPRINTF(DBGERR, "FDC_setFDfromCFG(): error when opening path '%s' and file '%s', DRIVE: 0x%x\n",
    			self->drive [ drive_id ].path, self->drive [ drive_id ].filename, drive_id );

        self->drive [ drive_id ].path [ 0 ] = '\0';
        self->drive [ drive_id ].filename [ 0 ] = '\0';

        return ( -1 );   // error when open DSK file
    };

    self->drive[ drive_id ].track_offset = FDC_GetTrackOffset ( self, drive_id, self->drive[ drive_id ].TRACK, self->drive[ drive_id ].SIDE );

    if ( ! self->drive[ drive_id ].track_offset ) {

    	DBGPRINTF(DBGERR, "FDC_setFDfromCFG(): FDC_GetTrackOffset - returned error!\n");

        f_close ( &(self->drive [ drive_id ].fh ));
        self->drive [ drive_id ].path [ 0 ] = '\0';
        self->drive [ drive_id ].filename [ 0 ] = '\0';

       return ( -1 );   // error when setting track 0 side 0
    };

    DBGPRINTF(DBGINF, "FDC_setFDfromCFG(): DRIVE: 0x%, new FH: 0x%x\n",
    		drive_id, self->drive [ drive_id ].fh);

    return ( 1 );
}

/*
 * FDC_Init() - vynulovani vsech hodnot ve strukture FDC a primountovani
 * DSK souboru podle konfigurace v /unicard/fd[0-3].cfg
 *
 */
int fdc_init (void *v_self)
{
  FDC *self = (FDC *)v_self;
  uint8_t i;

    //memset ( &FDC, 0x00, sizeof ( FDC ) );

	self->fd0disabled = -1;

    return 1;
}

/*
 * FDC_set_track() - nastavi mechaniku na pozadovanou stopu podle
 * hodnot v self->regTRACK a self->SIDE
 *
 * Vraci:
 *    0 - vse v poradku
 *   1 - doslo k chybe (stopa nenalezena)
 */
static uint8_t FDC_set_track (FDC *self) {
    int32_t track_offset;

    // Je potreba nastavovat, nebo uz je mechanika v pozadovanem stavu?
    if ( self->drive[ self->MOTOR & 0x03 ].TRACK != self->regTRACK || self->drive[ self->MOTOR & 0x03 ].SIDE != self->SIDE ) {

        // byla nastavena jinak, takze ji musime prenastavit

        self->drive[ self->MOTOR & 0x03 ].SECTOR = 0;
        self->drive[ self->MOTOR & 0x03 ].sector_size = 0;

        track_offset = FDC_GetTrackOffset ( self, self->MOTOR & 0x03, self->regTRACK, self->SIDE );

        if ( track_offset ) {
            self->drive[ self->MOTOR & 0x03 ].track_offset = track_offset;
            self->drive[ self->MOTOR & 0x03 ].TRACK = self->regTRACK;
            self->drive[ self->MOTOR & 0x03 ].SIDE = self->SIDE;

        	DBGPRINTF(DBGINF, "FDC_set_track(): 0x%x\n", self->regTRACK );

        } else {

// TODO: set status to seek err !!!
        	DBGPRINTF(DBGERR, "FDC_set_track(): TRACK NOT FOUND DRIVE: 0x%x, TRACK: DEC %d, SIDE: 0x%x\n,",
					self->MOTOR & 0x03, self->regTRACK, self->SIDE);

            return ( 1 );
        };
    };

    return ( 0 );
}











/* main write function called from request decoder in main.c
 *
 */
int fdc_write(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr)
{
   FDC *self = (FDC *)v_self;
   unsigned char off = port&0x07;
   unsigned int ff_readlen;
   uint8_t *io_data = &dt;

  /* Connection is like following:
   * offset = 0 - command/status reg
   * offset = 1 - track reg
   * offset = 2 - sector reg
   * offset = 3 - data reg
   * offset = 4 - FDD motor reg
   * offset = 5 - side reg
   * offset = 6 - DD/HD switch
   * offset = 7 - IRQ enable/disable
   */
    switch (off) {
    case 0 :
                // pokud jsme byli v INT rezimu, tak jej vyresetujeme
                if ( self->waitForInt ) {
                    self->waitForInt = 0;

                	DBGPRINTF(DBGINF, "SharpINT => OFF!\n");

                    release_interrupt();
                };


                self->COMMAND = *io_data;

                self->reading_status_counter = 0;

                // Comnand type I.
                if ( self->COMMAND & 0x80 ) {


                    // Before all type I. commands:
                    self->regSTATUS = 0x00;

                    // empty drive
                    if ( ! self->drive[ self->MOTOR & 0x03 ].fh.obj.fs ) {

                    	DBGPRINTF(DBGWAR, "Empty drive 0x%x, NOT READY for command type I.\n",
                    			self->MOTOR & 0x03);

                        self->regSTATUS = 0x80;   // not ready
                        return RETURN_FDC_ERR;
                    };

                    // RESTORE
                    if ( self->COMMAND >> 4 == 0x0F ) {

                    	DBGPRINTF(DBGWAR, "FDC do COMMAND: 0x%x - RESTORE, DRIVE: 0x%x\n",
                    			self->COMMAND, self->MOTOR & 0x03 );

                        self->regTRACK = 0;
                        self->SIDE = 0;

                    // SEEK
                    } else if ( self->COMMAND >> 4 == 0x0E ) {

                    	DBGPRINTF(DBGWAR, "FDC do COMMAND: 0x%x - SEEK, DRIVE: 0x%x, TRACK: 0x\n",
                    			self->COMMAND, self->MOTOR & 0x03, self->regTRACK);

                        self->regTRACK = self->regDATA;

                        DBGPRINTF(DBGWAR, "SEEK - TRACK-after: 0x\n", self->regTRACK );

                    // STEP IN (track +1)
                    } else if ( self->COMMAND >> 5 == 0x05 ) {

                    	DBGPRINTF(DBGWAR, "FDC do COMMAND: 0x%x - STEP IN (track + 1), DRIVE: 0x%x, TRACK: 0x%x\n",
                    			self->COMMAND, self->MOTOR & 0x03, self->regTRACK);

                        self->regTRACK++;
                        self->STATUS_SCRIPT = 1;

                    	DBGPRINTF(DBGWAR, "STEP IN - TRACK-after: 0x\n", self->regTRACK );


                    // STEP OUT (track -1)
                    } else if ( self->COMMAND >> 5 == 0x04 ) {

                    	DBGPRINTF(DBGINF, "FDC do COMMAND: 0x%x - STEP OUT (track - 1), DRIVE: 0x%x, TRACK: 0x\n",
                    			self->COMMAND, self->MOTOR & 0x03, self->regTRACK );

                    	if ( self->regTRACK ) {
                            self->regTRACK--;
                        };

                    	DBGPRINTF(DBGINF, "STEP OUT - TRACK-after: 0x\n", self->regTRACK );

                    };


                    // After all type I. commands:
                    self->COMMAND = 0x00;
                    self->DATA_COUNTER = 0;
                    self->buffer_pos = 0;
                    if ( self->regTRACK == 0 ) {
                        self->regSTATUS |= 0x04; // TRC00
                    };
                    self->STATUS_SCRIPT = 1; // one BUSY, next READY
                    return RETURN_FDC_OK;


                // Command type II.
                } else if ( self->COMMAND >> 6 == 0x01 ) {

                    // Before all type II. commands:
                    self->regSTATUS = 0;
                    self->DATA_COUNTER = 0;
                    self->buffer_pos = 0;
                    self->STATUS_SCRIPT = 1;

                    // empty drive
                    if ( ! self->drive[ self->MOTOR & 0x03 ].fh.obj.fs ) {

                    	DBGPRINTF(DBGINF, "Empty drive 0x%x, NOT READY for command type II.\n",
                    			self->MOTOR & 0x03 );

                    	self->regSTATUS = 0x80;   // not ready
                        return RETURN_FDC_ERR;
                    };

                    if ( self->COMMAND & 0x10 ) {
                        self->MULTIBLOCK_RW = 0;
                    } else {
                        self->MULTIBLOCK_RW = 1;
                    };

                    if ( FDC_set_track(self) ) {
                        // stopa nenalezena, melo by se to nejak osetrit statuskodem
                        return RETURN_FDC_ERR;
                    };

#if FAT_WRITE_SUPPORT
                    // READ SECTOR,  WRITE SECTOR
                    if ( self->COMMAND >> 5 == 0x03 || self->COMMAND >> 5 == 0x02 ) {
#else
                    // READ SECTOR,  WRITE SECTOR
                    if ( self->COMMAND >> 5 == 0x03 ) {
#endif


                    	DBGPRINTF(DBGINF, "FDC do COMMAND: 0x%x\n", self->COMMAND );

#if FAT_WRITE_SUPPORT
                        if ( self->COMMAND >> 5 == 0x02 ) {

                        	DBGPRINTF(DBGINF, " - WRITE SECTOR\n");

                        } else {
#endif

                        	DBGPRINTF(DBGINF, " - READ SECTOR\n");

#if FAT_WRITE_SUPPORT
                        };
#endif
                        if ( self->MULTIBLOCK_RW ) {

                        	DBGPRINTF(DBGINF, " (multiblock)\n");

                        };

                        DBGPRINTF(DBGINF, ", DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x\n",
                        self->MOTOR & 0x03, self->drive[ self->MOTOR & 0x03 ].TRACK, self->drive[ self->MOTOR & 0x03 ].SIDE, self->regSECTOR );



                        if ( ! self->drive[ self->MOTOR & 0x03 ].track_offset ) {
// status code set to track error ?
                            self->STATUS_SCRIPT = 3;
                            return RETURN_FDC_ERR;
                        };

                        if ( FDC_SeekToSector ( self, self->MOTOR & 0x03, self->regSECTOR ) ) {
                            // sector not found!
                            self->STATUS_SCRIPT = 3;	// LEC cp/m v1.3 specs ...
                            return RETURN_FDC_ERR;
                        };

                        if ( self->COMMAND >> 5 == 0x03 ) {
                            uint16_t fdd_io_size;
                            if ( self->drive[ self->MOTOR & 0x03 ].sector_size < sizeof ( self->buffer ) ) {
                                fdd_io_size = self->drive[ self->MOTOR & 0x03 ].sector_size;
                            } else {
                                fdd_io_size = sizeof ( self->buffer );
                            };


                            f_read( &(self->drive[ self->MOTOR & 0x03 ].fh), self->buffer, fdd_io_size, &ff_readlen );
                            if ( ff_readlen != fdd_io_size ) {

                            	DBGPRINTF(DBGERR, "FDControllerMain(): error when reading2 DSK file!\n");
                            	// readsize = 0x%x", readsize );
                            	DBGPRINTF(DBGERR, "fdd_io_size = 0x%x, ff_readlen = 0x%x\n", fdd_io_size, ff_readlen);

                            	DBGPRINTF(DBGERR, "DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, track_offset: 0x%x\n",
                            			0x30+(self->MOTOR & 0x03),
                            			0x30+self->drive[ self->MOTOR & 0x03 ].TRACK,
                            			0x30 + self->drive[ self->MOTOR & 0x03 ].SIDE,
                            			self->drive[ self->MOTOR & 0x03 ].SECTOR,
                            			self->drive[ self->MOTOR & 0x03 ].track_offset );


                                return RETURN_FDC_ERR;
                            };



                        };

                        self->DATA_COUNTER = self->drive[ self->MOTOR & 0x03 ].sector_size;
                        self->regSTATUS |= 0x01; // BUSY
                        self->regSTATUS |= 0x02; // DRQ


                    }  else {

                    	DBGPRINTF(DBGINF, "????? NOT IMPLEMENTED COMMAND Type II. COMMAND: 0x%x\n", *io_data );

                    };

                // Command type III. - read track addr
                } else if ( self->COMMAND >> 4 == 0x03 ) {
//                } else if ( self->COMMAND == 0x3f ) {

// TODO: empty drive

                    if ( FDC_set_track(self) ) {
                        // stopa nenalezena
                        return RETURN_FDC_ERR;
                    };

                    if ( ! self->drive[ self->MOTOR & 0x03 ].SECTOR || ! self->drive[ self->MOTOR & 0x03 ].sector_size ) {
                        if ( FDC_SeekToSector ( self, self->MOTOR & 0x03, 1 ) ) {
                            // sektor nenalezen
                            return RETURN_FDC_ERR;
                        };
                    };
                    self->regSECTOR = self->drive[ self->MOTOR & 0x03 ].SECTOR;
                    self->buffer [ 0 ] = self->drive[ self->MOTOR & 0x03 ].TRACK;
                    self->buffer [ 1 ] = self->drive[ self->MOTOR & 0x03 ].SECTOR;
                    self->buffer [ 2 ] = self->drive[ self->MOTOR & 0x03 ].SIDE;
                    self->buffer [ 3 ] = self->drive[ self->MOTOR & 0x03 ].sector_size / 0x100;
                    self->buffer [ 4 ] = 0x00;
                    self->buffer [ 5 ] = 0x00;

                    self->DATA_COUNTER = 6;
                    self->regSTATUS = 0x00;
                    self->regSTATUS |= 0x01; // BUSY
                    self->regSTATUS |= 0x02; // DRQ
                    self->STATUS_SCRIPT = 1;


#if FAT_WRITE_SUPPORT
                // Command type III. - write track (format)
                } else if ( self->COMMAND == 0x0f || self->COMMAND == 0x0b ) {


                	DBGPRINTF(DBGINF, "FDC Command WRITE TRACK 0x", self->COMMAND );


                    if ( ! self->drive[ self->MOTOR & 0x03 ].fh.obj.fs ) {

                    	DBGPRINTF(DBGWAR, "Empty drive 0x%x, NOT READY for command type III. - WRITE TRACK\n", self->MOTOR & 0x03 );

                        self->regSTATUS = 0x80;   // not ready
                        return RETURN_FDC_ERR;
                    };

// TODO: zkratit soubor, smazat tabulku stop a vynulovat pocet stop (??)

                    self->write_track_stage = 0;
                    self->write_track_counter = 0;

                    self->regSTATUS = 0x00;
                    self->regSTATUS |= 0x01; // BUSY
                    self->regSTATUS |= 0x02; // DRQ
                    self->STATUS_SCRIPT = 1;

#endif // FAT_WRITE_SUPPORT


                // Command type IV. - interrupts
                } else if ( self->COMMAND == 0x27 || self->COMMAND == 0x2f ) {


                	DBGPRINTF(DBGINF, "FDC Command INTERRUPT 0x%x\n", self->COMMAND );


                	self->DATA_COUNTER = 0;
                    self->buffer_pos = 0;
                    self->COMMAND = 0x00;
                    self->regSTATUS = 0x00;
                    self->STATUS_SCRIPT = 0;


                } else {

                	DBGPRINTF(DBGINF, "????? UNKNOWN COMMAND Type ???. COMMAND: 0x%x\n", *io_data );


                };
      break;

    case 1 :
                // cp/m testuje jaka je aktualni stopa tak, ze do track registru zapise 0x00
                // a nasledne jej cte a ocekava tam aktualni stopu na ktere je mechanika.
                // Takze pokud chce nekdo nastavit stopu 0, tak mu to nepovolime, protoze
                // pak by cp/m neustale seekovala.
                if ( *io_data != 0xff ) {

                	self->regTRACK = ~*io_data;

                	DBGPRINTF(DBGINF, "FDC Set regTRACK: 0x%x\n", self->regTRACK );


                } else {

                	DBGPRINTF(DBGINF, "FDC Set regTRACK (ignored value!): 0x\n");

                };
      break;

    case 2 :
                self->regSECTOR = ~*io_data;

                DBGPRINTF(DBGINF, "FDC Set regSECTOR: 0x\n", self->regSECTOR );

      break;

    case 3 :
            // pokud byl pro MZ-800 vystaven /INT, tak jej deaktivujeme
            if ( self->waitForInt ) {
                self->waitForInt = 0;

                DBGPRINTF(DBGINF, "SharpINT => OFF!\n");

                release_interrupt();
            };

            self->reading_status_counter = 0;

                self->reading_status_counter = 0;

#if FAT_WRITE_SUPPORT
                // probiha formatovani?
                if ( self->COMMAND == 0x0f || self->COMMAND == 0x0b )  {


#if (DBGLEVEL == DBGINF)
                    uint8_t last_write_track_stage = self->write_track_stage;
#endif

                    // jsme na zacatku zapisu stopy - cekame na indexovou znacku
                    if ( self->write_track_stage == 0 ) {


                    	DBGPRINTF(DBGINF, "WRITE TRACK - waiting for index\n");


                    	// prisel pocatecni index, takze budeme opravdu formatovat :)
                        if ( *io_data == 0x03 ) {	// ~0xfc
                            self->write_track_stage = 1;
                            self->write_track_counter = 0;


                            // u prvni stopy upravime hlavicku DSK
                            // a prepiseme tabulku stop
                            if ( self->regTRACK == 0 && self->SIDE == 0 ) {
                                int32_t write_track_offset = 0x22;
                                uint8_t write_need = 204;
                                uint16_t write_length;
                                if ( ! f_lseek ( &self->drive[ self->MOTOR & 0x03 ].fh, write_track_offset ) ) {
// TODO: err status
                                    self->regSTATUS = 0x00;
                                    self->STATUS_SCRIPT = 0;
                                    self->COMMAND = 0x00;
                                    return RETURN_FDC_ERR;
                                };
                                if ( FR_OK != f_write ( &self->drive[ self->MOTOR & 0x03 ].fh, (uint8_t *) &"Unicard v1.00\0\0\2\0\0", 18, &ff_readlen )) {
// TODO: err status
                                    self->regSTATUS = 0x00;
                                    self->STATUS_SCRIPT = 0;
                                    self->COMMAND = 0x00;
                                    return RETURN_FDC_ERR;
                                };
				if ( 18 !=  ff_readlen ) {
                                    self->regSTATUS = 0x00;
                                    self->STATUS_SCRIPT = 0;
                                    self->COMMAND = 0x00;
                                    return RETURN_FDC_ERR;
				}
                                memset ( &self->buffer, 0x00, sizeof ( self->buffer ) );

                                while ( write_need ) {
                                    if ( write_need > sizeof ( self->buffer ) ) {
                                        write_length = sizeof ( self->buffer );
                                        write_need -= sizeof ( self->buffer );
                                    } else {
                                        write_length = write_need;
                                        write_need = 0;
                                    };
				    f_write ( &self->drive[ self->MOTOR & 0x03 ].fh, self->buffer, write_length, &ff_readlen);
				    if ( ff_readlen != write_length ) {
// TODO: err status
                                        self->regSTATUS = 0x00;
                                        self->STATUS_SCRIPT = 0;
                                        self->COMMAND = 0x00;
                                        return RETURN_FDC_ERR;
                                    };
                                };

                            } else {
                                memset ( &self->buffer, 0x00, sizeof ( self->buffer ) );
                            };


                        // melo se formatovat, ale nikdo s tim nezacal !
                        } else if ( self->write_track_counter > 100 ) {
// TODO: data lost
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                            self->COMMAND = 0x00;
                            return RETURN_FDC_ERR;
                        };


                    // cekame az prijde identifikacni znacka
                    } else if ( self->write_track_stage == 1 ) {

                        // prisla identifikacni znacka
                        if ( *io_data == 0x01 ) {	// ~0xfe
                            self->write_track_stage = 2;
                            self->write_track_counter = 0;

                            // zacina fyzicky prvni sektor na stope
                            // docasnou tabulku sektoru si vytvorime ve self->buffer
                            self->buffer [ 0 ] = self->regTRACK;
                            self->buffer [ 1 ] = self->SIDE;
                            // 2 - 3 unused, 4 sector size,
                            // 5 number of sectors
                            self->buffer [ 5 ] = 1;
                            //6 GAP#3 length, 7 filler byte
                            self->buffer [ 6 ] = 0x4e;
                            self->buffer [ 7 ] = 0xe5;
                            self->buffer_pos = 8; // tady uz pokracuje pouze seznam ID pro jednotlive sektory

                        // neobdrzeli jsme identifikacni znacku
                        } else if ( self->write_track_counter > 100 ) {
// TODO: data lost
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                            self->COMMAND = 0x00;
                            return RETURN_FDC_ERR;
                        };

                    // po identifikacni znacce si precteme
                    // stopu, stranu, sektor a delku sektoru
                    } else if ( self->write_track_stage == 2 ) {
                        if ( self->write_track_counter <= 4 ) {
                            // ulozime si ID sektoru
                            if ( self->write_track_counter == 3 ) {
                                self->buffer [ self->buffer_pos++ ] = ~*io_data;

                            // ulozime si velikost sektoru
                            // predpokladame, ze velikost vsech sektoru na stope musi byt stejna
                            } else if ( self->write_track_counter == 4 ) {
                                self->buffer [ 4 ] = ~*io_data;
                            };

                        // cekame na znacku dat
                        } else if ( *io_data == 0x04 || *io_data == 0x07 )  {	// 0xfb
                            self->write_track_stage = 3;
                            self->write_track_counter = 0;
                            self->DATA_COUNTER = self->buffer [ 4 ] * 0x0100;

                        // znacka dat neprisla
                        } else if ( self->write_track_counter > 100 ) {
// TODO: data lost
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                            self->COMMAND = 0x00;
                            return RETURN_FDC_ERR;
                        };

                    // cteme obsah formatovaneho sektoru
                    } else if ( self->write_track_stage == 3 ) {

                        // prvni bajt si ulozime
                        if ( self->write_track_counter == 1 ) {
                            self->buffer [ sizeof ( self->buffer ) - 1 ] = ~*io_data;
                        };

                        // posledni bajt sektoru?
                        if ( self->write_track_counter > self->DATA_COUNTER ) {
                            self->write_track_stage = 4;
                            self->write_track_counter = 0;
                            self->DATA_COUNTER = 0;


                            DBGPRINTF(DBGINF, "WRITE TRACK - finished sector field DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, SIZE: 0x%x, value: 0x%x\n",
                            		self->MOTOR & 0x03,
                            		self->regTRACK,
                            		self->SIDE,
                            		self->buffer [ self->buffer_pos - 1 ],
                            		self->buffer [ 4 ],
                            		self->buffer [ sizeof ( self->buffer ) - 1 ] );

                        };

                    // zapis do sektoru skoncil, tak cekame zda prijde dalsi,
                    // nebo zda uz je konec stopy
                    } else if ( self->write_track_stage == 4 ) {
                        if ( *io_data == 0x01 ) {	//0xfe
                            self->write_track_stage = 2;
                            self->write_track_counter = 0;
                            self->buffer [ 5 ]++;	// zvysime cislo s informaci o poctu sektoru na stope

                        // zrejme konec stopy
                        } else if ( self->write_track_counter > 200 ) {
                            uint8_t i;
                            uint8_t all_sec_size = 0;

														uint16_t write_need = all_sec_size * 0x0100;
                            uint16_t write_length;

                            int32_t offset = 0x30;	// info o poctu stop

														self->COMMAND = 0x00;
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                            self->write_track_stage = 5;

                            DBGPRINTF(DBGINF, "WRITE TRACK - finishing DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x\n",
                            		self->MOTOR & 0x03,
                            		self->regTRACK,
                            		self->SIDE );


                            // zapiseme stopu do DSK

                            if ( FDC_set_track(self) ) return RETURN_FDC_ERR; // stopa nenalezena
// TODO: err status

                            DBGPRINTF(DBGINF, "new track offset: 0x%x\n", self->drive[ self->MOTOR & 0x03 ].track_offset );


                            if ( FR_OK != f_lseek ( &self->drive[ self->MOTOR & 0x03 ].fh, self->drive[ self->MOTOR & 0x03 ].track_offset) ) {
// TODO: err sts
                                return RETURN_FDC_ERR;
                            };

			    f_write ( &self->drive[ self->MOTOR & 0x03 ].fh,(uint8_t *) &"Track-Info\x0d\x0a\0\0\0\0", 16, &ff_readlen );
                            if ( ff_readlen != 16 ) return RETURN_FDC_ERR; // write error
// TODO: err status
			    f_write ( &self->drive[ self->MOTOR & 0x03 ].fh, self->buffer, 8, &ff_readlen);
                            if ( ff_readlen != 8 ) return RETURN_FDC_ERR; // write error
// TODO: err status

                            self->buffer_pos = 8; // opet na zacatek tabulky sektoru
                            // zapiseme info o kazdem sektoru
                            self->buffer [ 3 ] = self->buffer [ 4 ]; // sector size
                            self->buffer [ 4 ] = 0x00; // (info z DSK) FDC status register 1
                            self->buffer [ 5 ] = 0x00; // (info z DSK) FDC status register 2
                            self->buffer [ 6 ] = 0; //( self->buffer [ 3 ] * 0x0100 ) & 0xff; // skutecna velikost sektoru spodni bajt
                            self->buffer [ 7 ] = ( self->buffer [ 3 ]); // * 0x0100 ) >> 8; // skutecna velikost sektoru horni bajt

                            for ( i = 1; i <= 29; i++ ) {
                                if ( self->buffer_pos != 0 ) {
                                    if ( self->buffer [ self->buffer_pos ] != 0x00 ) {
                                        self->buffer [ 2 ] = self->buffer [ self->buffer_pos ];
                                        self->buffer_pos++;
                                        all_sec_size += self->buffer [ 3 ];
                                    } else {
                                        memset ( self->buffer, 0x00, 8 );
                                        self->buffer_pos = 0;
                                    };
                                };
				f_write ( &self->drive[ self->MOTOR & 0x03 ].fh, self->buffer, 8, &ff_readlen);
				if ( ff_readlen != 8 ) return RETURN_FDC_ERR; // write error

// TODO: err status
                            };

                            // fyzicky zapis vsech sektoru na stope
                            memset ( &self->buffer, self->buffer [ sizeof ( self->buffer ) - 1 ], sizeof ( self->buffer ) );

                            while ( write_need ) {
                                if ( write_need > sizeof ( self->buffer ) ) {
                                    write_length = sizeof ( self->buffer );
                                    write_need -= sizeof ( self->buffer );
                                } else {
                                    write_length = write_need;
                                    write_need = 0;
                                };
				f_write ( &self->drive[ self->MOTOR & 0x03 ].fh, self->buffer, write_length, &ff_readlen);
				if ( ff_readlen != write_length ) return RETURN_FDC_ERR; // write error
// TODO: err status
                            };

                            // urizneme konec DSK souboru
/*                             int32_t file_pos = MySD_get_file_pos ( self->drive[ self->MOTOR & 0x03 ].fh ); */

/*                             int32_t offset = 0; */
/*                             if ( FR_OK != f_lseek ( &self->drive[ self->MOTOR & 0x03 ].fh, offset) ) { */
/* // TODO: err sts */
/*                                 return RETURN_FDC_ERR; */
/*                             }; */

                            if ( FR_OK != f_sync(&self->drive[ self->MOTOR & 0x03].fh) ) {
// TODO: err stat
                                return RETURN_FDC_ERR;
                            };

                            if ( FR_OK != f_truncate ( &self->drive[self->MOTOR&0x03].fh) ) {
// TODO: err stat
                                return RETURN_FDC_ERR;
                            };

                            if ( FR_OK != f_sync(&self->drive[ self->MOTOR & 0x03].fh) ) {
// TODO: err stat
                                return RETURN_FDC_ERR;
                            };


                            // upravime tabulku stop

                            if ( FR_OK != f_lseek( &self->drive[self->MOTOR&0x03].fh, offset) ) return RETURN_FDC_ERR; // seek error
// TODO: err stat
                            if ( self->SIDE == 1 ) {
                                self->buffer [ 0 ] = self->regTRACK + 1;
                            } else {
                                self->buffer [ 0 ] = self->regTRACK;
                            };
			    f_write ( &self->drive[self->MOTOR&0x03].fh, self->buffer, 1, &ff_readlen);
			    if ( ff_readlen != 1 ) return RETURN_FDC_ERR; // write error
// TODO: err status
                            // velikost ulozene stopy
                            self->buffer [ 0 ] = all_sec_size + 1;
                            offset = 0x34 + ( self->regTRACK *2 ) + self->SIDE ;
                            if ( FR_OK != f_lseek ( &self->drive[self->MOTOR&0x03].fh, offset) ) return RETURN_FDC_ERR; // seek error
// TODO: err stat
			    f_write ( &self->drive[self->MOTOR&0x03].fh, self->buffer, 1, &ff_readlen);
			    if ( ff_readlen != 1 ) return RETURN_FDC_ERR; // write error
// TODO: err status
                            if ( FR_OK != f_sync(&self->drive[self->MOTOR&0x03].fh) ) {
                                return RETURN_FDC_ERR; // TODO: err stat
                            };


                            DBGPRINTF(DBGINF, "WRITE TRACK - finished\n");


                        };
                    };

#if (DBGLEVEL == DBGINF)

                    if ( last_write_track_stage != self->write_track_stage ) {

                    	DBGPRINTF(DBGINF, "WRITE TRACK - stage changed 0x\n", self->write_track_stage );

                    };
#endif


                    self->write_track_counter++;
                    return RETURN_FDC_OK;
                };
#endif


                // Write byte only into DATA register
                if ( ! self->DATA_COUNTER ) {

                    self->regDATA = ~*io_data;

                    DBGPRINTF(DBGINF, "FDC SET regDATA: 0x%x\n", self->regDATA );

                    return RETURN_FDC_OK;
                };


#if FAT_WRITE_SUPPORT
                // WRITE sector

                // empty drive
                if ( ! self->drive[ self->MOTOR & 0x03 ].fh.obj.fs ) {

                	DBGPRINTF(DBGINF, "Empty drive 0x%x, NOT READY for write data into sector.\n", self->MOTOR & 0x03 );

                    self->regSTATUS = 0x80;   // not ready
                    return RETURN_FDC_ERR;
                };

#if (DBGLEVEL == DBGINF)

                if ( self->DATA_COUNTER == self->drive[ self->MOTOR & 0x03 ].sector_size ) {

                	DBGPRINTF(DBGINF, "Writing first byte into FDD sector. Debug messages are suppressed.\n");
                };
#endif

                if ( self->DATA_COUNTER ) {
                    uint16_t fdd_io_size;
                    SUPPRESSED_DBGMSG = 1;

                    self->buffer [ self->buffer_pos ] = ~*io_data;

                    self->DATA_COUNTER--;

                    if ( self->drive[ self->MOTOR & 0x03 ].sector_size < sizeof ( self->buffer ) ) {
                        fdd_io_size = self->drive[ self->MOTOR & 0x03 ].sector_size;
                    } else {
                        fdd_io_size = sizeof ( self->buffer );
                    };
                    if ( self->buffer_pos == fdd_io_size - 1 ) {
                        int16_t retval=0;
                        self->buffer_pos = 0;
			f_write ( &self->drive[self->MOTOR&0x03].fh, self->buffer, fdd_io_size, &ff_readlen);
			if ( ff_readlen != fdd_io_size ) {

				DBGPRINTF(DBGERR, "FDControllerMain(): error when writing DSK file! retval: 0x%x\n", retval);

                        };
                    } else {
                        self->buffer_pos++;
                    };


                    if ( ! self->DATA_COUNTER ) {

                    	DBGPRINTF(DBGINF, "Writing last byte into FDD sector.\nDRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, track_offset: 0x%x\n",
                    			self->MOTOR & 0x03,
                    			self->drive[ self->MOTOR & 0x03 ].TRACK,
                    			self->drive[ self->MOTOR & 0x03 ].SIDE,
                    			self->drive[ self->MOTOR & 0x03 ].SECTOR,
                    			self->drive[ self->MOTOR & 0x03 ].track_offset );


                        if ( FR_OK != f_sync(&self->drive[self->MOTOR&0x03].fh) ) {
                            // TODO: err stat ?
                            DBGPRINTF(DBGERR, "FDControllerMain(): error syncing disk\n");
                        };


                        if ( self->MULTIBLOCK_RW ) {

                        	DBGPRINTF(DBGINF, "Multiblock sector writing - sector is finished. Go to next...\n");

                                self->regSECTOR = self->drive[ self->MOTOR & 0x03 ].SECTOR + 1;

                                if ( FDC_SeekToSector ( self, self->MOTOR & 0x03, self->regSECTOR ) ) {

                                	DBGPRINTF(DBGINF, "Not found next sector on this track. Sending RNF!\n");

                                    self->regSECTOR--;
                                    self->STATUS_SCRIPT = 4;

                                } else {
                                    self->DATA_COUNTER = self->drive[ self->MOTOR & 0x03 ].sector_size;
                                    self->buffer_pos = 0;
                                    self->STATUS_SCRIPT = 2;
                                };

                        } else {
                            self->COMMAND = 0x00;
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                        };
                    };
                };

#endif  // FAT_WRITE_SUPPORT
      break;

    case 4 :
                // ID mechaniky (0. - 1. bit) se zmeni jen pokud je nataven i 2. bit
                // 7. bit zapina/vypina motor
                if ( 0x04 == ( *io_data & 0x04 ) ) {
                    self->MOTOR = *io_data & 0x83;
                } else {
                    if ( 0x80 == ( *io_data & 0x80 ) ) {
                        self->MOTOR = self->MOTOR | 0x80;
                    } else {
                        self->MOTOR = self->MOTOR & 0x03;
                    };
                };
                DBGPRINTF(DBGINF, "FDC Set MOTOR: 0x%x\n", self->MOTOR);

      break;

    case 5 :
                self->SIDE = *io_data & 0x01;

                DBGPRINTF(DBGINF, "FDC Set head SIDE: 0x%x\n", self->SIDE );

      break;

    case 6 :
                self->DENSITY = *io_data & 0x01;
                DBGPRINTF(DBGINF, "FDC Set data DENSITY: 0x\n", self->DENSITY );


                break;

    case 7 :
                self->EINT = *io_data & 0x01;

                // pokud uz neni o INT rezim zajem, tak vypneme /INT signal
                if ( ! self->EINT ) {
                    self->waitForInt = 0;

                	DBGPRINTF(DBGINF, "SharpINT => OFF!\n");

                   release_interrupt();

                };

                DBGPRINTF(DBGINF, "FDC Set INTerrupt mode: 0x%x\n", self->EINT );

      break;

    default:
       break;
    }

  return 0;
}












/* main read function called from request decoder in main.c
 *
 */
int fdc_read(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr)
{
   FDC *self = (FDC *)v_self;
   unsigned char off = port&0x07;
   unsigned int ff_readlen;
   unsigned int readsize = 0;

    switch( off) {
    case 0 :
      // Pokud startuje cp/m a testuje si status po nastaveni neexistujiciho sectoru, tak si na zadny timeout nehrajeme.
 	if ( self->regSTATUS != 0x18 ) {

 	                // Hack pro diskovy MZ-800 BASIC, ktery po zapisu provadi overeni ctenim
                // sektoru v multiblokovem rezimu, ale samotna data necte a pouze sleduje
                // zda ve status registru neprijde chyba - dokud multiblokovym ctenim radic
                // neprojde vsechny kontrolovane sektory
                // Pokud je tedy nastaveno multiblokove cteni a 10x se MZ-800 zeptal na status
                // a necetl zadne data, tak automaticky prejdeme na dalsi sektor.
                //
                // Stejny mechanismus verifikace je pouzity i v cp/m 4.1 format4.com, akorat se
                // pouziva jednoblokove cteni.
                if ( ( self->DATA_COUNTER == self->drive[ self->MOTOR & 0x03 ].sector_size ) && ( self->COMMAND >> 5 == 0x03 ) ) {
                    self->reading_status_counter++;
                    if ( self->reading_status_counter > 10 ) {
                        self->reading_status_counter = 0;

                        // probiha multiblokove cteni - prejdeme na dalsi sektor
                        if ( self->MULTIBLOCK_RW ) {

                        	DBGPRINTF(DBGINF, "Sekvencni cteni - predchozi sektor skoncil TIMEOUTEM , tzn. prechod na dalsi.\n");

                            self->regSECTOR = self->drive[ self->MOTOR & 0x03 ].SECTOR + 1;

                            if ( FDC_SeekToSector ( self, self->MOTOR & 0x03, self->regSECTOR ) ) {
                                // sector not found!

                            	DBGPRINTF(DBGINF, "Dalsi sector s naslednym poradovym cislem uz tu neni! Posilam RNF\n");

                                self->regSECTOR--;
                                self->STATUS_SCRIPT = 4;
                            } else {

                                uint16_t fdd_io_size;
                                if ( self->drive[ self->MOTOR & 0x03 ].sector_size < sizeof ( self->buffer ) ) {
                                    fdd_io_size = self->drive[ self->MOTOR & 0x03 ].sector_size;
                                } else {
                                    fdd_io_size = sizeof ( self->buffer );
                                };

                                f_read( &(self->drive[ self->MOTOR & 0x03 ].fh), self->buffer, fdd_io_size, &ff_readlen );
                                if ( ff_readlen != fdd_io_size ) {

                                	DBGPRINTF(DBGERR, "FDControllerMain(): error when reading2 DSK file! readsize = 0x%x\n", readsize );


                                	DBGPRINTF(DBGINF, "DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, track_offset: 0x%x\n",
                                			self->MOTOR & 0x03,
                                			self->drive[ self->MOTOR & 0x03 ].TRACK,
											self->drive[ self->MOTOR & 0x03 ].SIDE,
											self->drive[ self->MOTOR & 0x03 ].SECTOR,
											self->drive[ self->MOTOR & 0x03 ].track_offset );

// status code?
                                    return RETURN_FDC_ERR;
                                };


                                self->buffer_pos = 0;
                                self->DATA_COUNTER = self->drive[ self->MOTOR & 0x03 ].sector_size;

                                self->STATUS_SCRIPT = 2;
                            };

                        // probiha jednoblokove cteni - vyrobime "konec sektoru"
                        } else {

                        	DBGPRINTF(DBGINF, "Cteni sektoru ukonceno TIMEOUTEM.\n");

                            self->DATA_COUNTER = 0;
                            self->COMMAND = 0x00;
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                        };
                    };
		};
	};



                switch ( self->STATUS_SCRIPT ) {

                    case 1:
                        // Status after normal commands
                        // on first status reading put BUSY and on all next readings put real status code
                        *dt = ~ ( self->regSTATUS | 0x01 );
                        self->STATUS_SCRIPT = 0;
                        break;

                    case 2:
                        // Status code after multiblock R/W sector finished and go to next
                        // 1x BUSY, and next on reading BUSY + DRQ
                        *dt = ~ 0x01;   // BUSY
                        self->regSTATUS = 0x03; // BUSY + DRQ
                        self->STATUS_SCRIPT = 0;
                        break;

                    case 3:
                        // hack for cp/m 1.3, when cp/m on startup is requested reading from unknown sector id
                        *dt = ~ 0x01; // BUSY
                        self->regSTATUS = 0x18;
                        self->STATUS_SCRIPT = 0;
                        break;

                    case 4:
                        // Status code after multiblock R/W sector finished and next not found
                        // 1x BUSY + RNF, and next 0x00
                        *dt = ~ 0x11; // BUSY + RNF
                        self->regSTATUS = 0x00;
                        self->STATUS_SCRIPT = 0;
                        break;

                    case 0xff:
                        // experimental script for hacking
                        //fdc.STATUS = 0x00; // -TRK00
                        *dt = ~ ( self->regSTATUS & ~0x06 );
                        self->regSTATUS++;
                        break;

                    default:
                        *dt = ~self->regSTATUS;
                };

#if (DBGLEVEL == DBGINF)
                // suppressed debug messages when reading data from sector
                // regSTATUS == 0x18 is when cp/m 1.3 starting
                // ( ( self->COMMAND == 0x0f || self->COMMAND == 0x0b ) == WRITE TRACK
                if ( ! ( ( SUPPRESSED_DBGMSG == 1 ) || ( self->regSTATUS == 0x18 ) || ( self->COMMAND == 0x0f || self->COMMAND == 0x0b ) ) ) {

                	DBGPRINTF(DBGINF, "FDC Get regSTATUS: 0x%x\n", ~*dt );

                };
#endif

      break;

    case 1 :
       if ( self->regTRACK == 0x5a ) {
           if ( ! self->drive[ 0 ].fh.obj.fs  ) {
               DBGPRINTF(DBGINF, "FDC Get regTRACK - We lie because drive 'A' is empty : 0x\n");
               *dt = 0xff;
	    } else {
	        FIL ff_file;
	        if (self->fd0disabled<0) {
	        	if ( FR_OK == f_open ( &ff_file, "/unicard/fd0disabled.cfg", FA_READ ) ) {
	            	f_close ( &ff_file );
	            	self->fd0disabled = 1;
				} else self->fd0disabled = 0;
			}
	        if (self->fd0disabled) {
	            DBGPRINTF(DBGINF, "FDC Get regTRACK - We lie because FDD boot is disabled by CFG : 0x\n");
	            *dt = 0xff;
                } else {
    	            DBGPRINTF(DBGINF, "FDC Get regTRACK: 0x");
                    *dt = ~self->regTRACK;
                };
	    };
       } else {
           DBGPRINTF(DBGINF, "FDC Get regTRACK: 0x");
           *dt = ~self->regTRACK;
       };
       break;
    case 2 :
                // pokud bezi motor, tak vracime stkutecny sektor nad kterym jsme
                // jinak vracime co, co si kdo nastavil do registru sektoru
                if ( self->MOTOR & 0x80 ) {
                	self->regSECTOR = self->drive[ self->MOTOR & 0x03 ].SECTOR;
                };
                *dt = ~self->regSECTOR;

            	DBGPRINTF(DBGINF, "FDC Get regSECTOR: 0x");

                break;
    case 3 :
            // pokud byl pro MZ-800 vystaven /INT, tak jej deaktivujeme
            if ( self->waitForInt ) {
                self->waitForInt = 0;

            	DBGPRINTF(DBGINF, "SharpINT => OFF!\n");

                release_interrupt();
            };

            self->reading_status_counter = 0;

                // empty drive
            if ( ! (self->drive[ self->MOTOR & 0x03 ].fh.obj.fs) ) {

            	DBGPRINTF(DBGINF, "Empty drive 0x%x, NOT READY for read from data register.\n", self->MOTOR & 0x03 );

                    self->regSTATUS = 0x80;   // not ready
                    *dt = 0xff;
                    return RETURN_FDC_ERR;
                };

                // requested DATA from TRACK ADDR
                if ( self->COMMAND == 0x3f ) {

                        *dt = ~self->buffer[ 6 - self->DATA_COUNTER ];

                    	DBGPRINTF(DBGINF, "TRACK ADDR READING (%x) - 0x%x\n",
                    			self->DATA_COUNTER - 1,
                    			self->buffer[ 6 - self->DATA_COUNTER ] );


                    	self->DATA_COUNTER--;

                        if ( ! self->DATA_COUNTER ) {
                            self->COMMAND = 0x00;
                            self->regSTATUS = 0x00;
                            self->STATUS_SCRIPT = 0;
                        };

                // requested DATA from SECTOR
                } else {

#if (DBGLEVEL == DBGINF)

                    if ( self->DATA_COUNTER == self->drive[ self->MOTOR & 0x03 ].sector_size ) {

                    	DBGPRINTF(DBGINF, "Requested first byte from FDD sector. Debug messages are suppressed.\n");

                    };
#endif

                    if ( self->DATA_COUNTER ) {

                      uint16_t fdd_io_size;

#if (DBGLEVEL == DBGINF)

                    	SUPPRESSED_DBGMSG = 1;
#endif

                        *dt = ~self->buffer[ self->buffer_pos ];

                        DBGPRINTF(DBGINF, " %x ", ~*dt);


                        self->DATA_COUNTER--;

                        if ( self->drive[ self->MOTOR & 0x03 ].sector_size < sizeof ( self->buffer ) ) {
                            fdd_io_size = self->drive[ self->MOTOR & 0x03 ].sector_size;
                        } else {
                            fdd_io_size = sizeof ( self->buffer );
                        };

                        if ( self->buffer_pos == fdd_io_size - 1 ) {
                            self->buffer_pos = 0;

                            f_read( &(self->drive[ self->MOTOR & 0x03 ].fh), self->buffer, fdd_io_size, &ff_readlen );
                            if ( ff_readlen != fdd_io_size ) {

                            	DBGPRINTF(DBGERR, "FDControllerMain(): error when reading3 DSK file! readsize = 0x%x\n", readsize );

                            	DBGPRINTF(DBGERR, "fdd_io_size = 0x%x, ff_readlen = 0x%x\n", fdd_io_size, ff_readlen);

                            	DBGPRINTF(DBGERR, "DRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, track_offset: 0x%x\n",
                            			self->MOTOR & 0x03,
                            			self->drive[ self->MOTOR & 0x03 ].TRACK,
                            			self->drive[ self->MOTOR & 0x03 ].SIDE,
                            			self->drive[ self->MOTOR & 0x03 ].SECTOR,
                            			self->drive[ self->MOTOR & 0x03 ].track_offset );

// status code?
                                return RETURN_FDC_ERR;
                            };
                        } else {
                            self->buffer_pos++;
                        };

                        if ( ! self->DATA_COUNTER ) {

#if (DBGLEVEL == DBGINF)
                            SUPPRESSED_DBGMSG = 0;
                        	DBGPRINTF(DBGINF, "Sector reading finished.\nDRIVE: 0x%x, TRACK: 0x%x, SIDE: 0x%x, SECTOR: 0x%x, track_offset: 0x%x\n",
                        			self->MOTOR & 0x03,
                        			self->drive[ self->MOTOR & 0x03 ].TRACK,
                        			self->drive[ self->MOTOR & 0x03 ].SIDE,
                        			self->drive[ self->MOTOR & 0x03 ].SECTOR,
                        			self->drive[ self->MOTOR & 0x03 ].track_offset );

#endif

                            if ( self->MULTIBLOCK_RW ) {
                                self->STATUS_SCRIPT = 2;
                                // hack for MZ BASIC
                                self->MULTIBLOCK_RW = 1;


                            	DBGPRINTF(DBGINF, "Multiblock sector reading - sector is finished. Go to next...\n");

                                self->regSECTOR = self->drive[ self->MOTOR & 0x03 ].SECTOR + 1;

                                if ( FDC_SeekToSector ( self, self->MOTOR & 0x03, self->regSECTOR ) ) {

                                	DBGPRINTF(DBGINF, "Not found next sector on this track. Sending RNF!\n");

                                	self->regSECTOR--;
                                    self->STATUS_SCRIPT = 4;

                                } else {
                                   f_read( &(self->drive[ self->MOTOR & 0x03 ].fh), self->buffer, fdd_io_size, &ff_readlen );
                                   if ( ff_readlen != fdd_io_size ) {

                                	   DBGPRINTF(DBGERR, "FDControllerMain(): error when reading4 DSK file!\n");

// err status
                                        return RETURN_FDC_ERR;
                                    };

                                    self->buffer_pos = 0;
                                    self->DATA_COUNTER = self->drive[ self->MOTOR & 0x03 ].sector_size;
                                    self->STATUS_SCRIPT = 2;
                                };

                            } else {
                                self->COMMAND = 0x00;
                                self->regSTATUS = 0x00;
                                self->STATUS_SCRIPT = 0;
                            };
                        };

                    } else {
                        // ma nekdo duvod cist data, kdyz si o zadne nerekl? :)
                    	DBGPRINTF(DBGERR, "ERROR?? MZ requested DATA from FDC, but DATA_COUNTER is empty!!!\n");
                    };
                };

       break;

    default:

    	DBGPRINTF(DBGWAR, "FDC UNKNOWN read on PORT offset %c\n",
    			0x30 + off );

       break;
    }

  return RETURN_FDC_OK;
}

int fdc_is_interrupt(void *v_self) {
    FDC *self = (FDC *)v_self;

    if ( ! self->EINT ) return 0; // radic neni v rezimu INT

    // Mame pripravena data ke cteni, nebo ocekavame data k zapisu?
#if FAT_WRITE_SUPPORT
    if ( ( self->DATA_COUNTER && ( self->COMMAND >> 5 == 0x03 || self->COMMAND >> 5 == 0x02 || self->COMMAND == 0x3f ) ) ||
    ( self->COMMAND == 0x0f || self->COMMAND == 0x0b ) ) {
#else
    if ( self->DATA_COUNTER && ( self->COMMAND >> 5 == 0b011 || self->COMMAND == 0x3f ) ) {
#endif

        // Signal /INT neposilame neustale, ale jen jednou za cas ...
        self->waitForInt++;

        // ... tedy po kazdem 2 pozadavku na Unicard
        if ( self->waitForInt > 2 ) {


        	DBGPRINTF(DBGINF, "FDC_doInterrupt(): SharpINT => ON!\n");


        	return 1;
        };

    };
    return 0;
}

FDC *fdc_new(uint8_t port_base) {
  FDC *fdc = calloc(1, sizeof *fdc);
  int i;
  for (i = 0; i<FDC_WRITE_PORTS; i++) {
    fdc->write[i].port = port_base + i;
    fdc->write[i].fn = fdc_write;
  }
  for (i = 0; i<FDC_READ_PORTS; i++) {
    fdc->read[i].port = port_base + i;
    fdc->read[i].fn = fdc_read;
  }

  fdc->iface.write = fdc->write;
  fdc->iface.write_port_count = FDC_WRITE_PORTS;

  fdc->iface.read = fdc->read;
  fdc->iface.read_port_count = FDC_READ_PORTS;
  fdc->iface.needs_exwait = 1;

  fdc->iface.init = fdc_init;
  fdc->iface.is_interrupt = fdc_is_interrupt;
  fdc->set_drive_content = fdc_set_drive_content;
  return fdc;
}


