#ifndef __FDC_H__
#define __FDC_H__

#include "ff.h"
#include "mz_devices.h"

#define FDC_WRITE_PORTS 8
#define FDC_READ_PORTS 4

int fdc_set_drive_content(void *v_self, uint8_t drive_id, char *file_path);


// pocet podporovanych FD mechanik (max 4)
#define FDC_NUM_DRIVES  4

// max. delka nazvu souboru na FAT32
#define FILENAME_LENGTH 32

/*
 * Struktura informaci udrzovanych pro kazdou FD mechaniku.
 *
 *
 */
typedef struct {
    char      path [ 3 * FILENAME_LENGTH + 3 ];   // cesta k DSK souboru
    char      filename [ FILENAME_LENGTH ];      // jmeno DSK souboru
    FIL       fh;               // filehandle otevreneho DSK souboru

    // aktualni stopa, sektor a strana na kterou jsme
    // momentalne v DSK souboru nastaveni
    uint8_t      TRACK;
    uint8_t      SECTOR;
    uint8_t      SIDE;         // bit0 urcuje nastavenou stranu

    int32_t      track_offset;      // Pozice aktualniho sektoru v DSK souboru
    int16_t      sector_size;      // Velikost aktualniho sektory v bajtech
} FDDrive;


/*
 * Struktura informaci udrzovanych pro kazdou FD radic.
 *
 *
 */
typedef struct {
   MZDevice iface;
   ReadPortMapping read[FDC_READ_PORTS];
   WritePortMapping write[FDC_WRITE_PORTS];
   int (*set_drive_content)(void *v_self, uint8_t drive_id, char *file_path);
    // registry FD radice
    uint8_t      regSTATUS;
    uint8_t      regDATA;


    uint8_t      regTRACK;
    uint8_t      regSECTOR;
    uint8_t      SIDE;         // bit0 urcuje pozadovanou stranu

    // datovy buffer pro cteni a zapis sektoru
    // Velikost bufferu musi byt takova, aby byl sektor delitelny velikosti bufferu
    //
    // Pri zapisu stopy pouzivame FDC.buffer, tak pokud se nekdo rozhodne,
    // ze zmensi velikost bufferu, tak by se mel nejprve podivat na WRITE TRACK, aby
    // mu potom kus bufferu nechybelo :)
    uint8_t      buffer [ 0x100 ];
    uint16_t      buffer_pos;      // ukazatel pozice v bufferu

    uint8_t      COMMAND;      // zde je ulozen FDC command, ktery jsme prijali na portu 0xd8
    uint8_t      MOTOR;         // bit0 a bit1 urcuje FD mechaniku se kterou prave pracujeme
                                                // bit7 zapina motor mechaniky (pouzivame pri cteni registru sektoru)
    uint8_t      DENSITY;
    uint8_t      EINT;         // 1 => INT rezim je povolen, 0 => zakazan
                                                // v pripade povoleneho INT rezimu rika radic signalem /INT,
                                                // ze ma pro MZ-800 pripraveny data

    uint16_t      DATA_COUNTER;      // Pri vykonavani prikazu cteni a zapisu sektoru, cteni adresy
                                                // sektoru je do tohoto citace vlozena velikost dat, ktere budou prijimany,
                                                // nebo odesilany. Tzn. velukost sektoru, nebo pocet bajt pri cteni adresy.
                                                // Pri kazdem cteni, nebo zapisu je tato hodnota snizovana.
                                                // Pokud je v tomto citaci nenulova hodnota, tak to znamena, ze radic ocekava
                                                // nejaka data v pripade zapisu, nebo ma pripravena data k odberu.

    FDDrive   drive [ FDC_NUM_DRIVES ]; // jednotlive mechaniky

    uint8_t      MULTIBLOCK_RW;      // 1 - znamena, ze posledni prikaz cteni/zapisu sektoru byl multiblokovy
                                                // tzn., ze az precteme / zapiseme posledni bajt aktualniho sektoru,
                                                // tak automaticky prejdeme na dalsi a pokracujeme dokud neprijde prikaz preruseni,
                                                // nebo dokud uz na stope neexistuje sektor s nasledujicim poradovym cislem
                                                // 0 - znamena obycejne cteni / zapis, ktere konci s poslednim bajtem sektoru.

    uint8_t      STATUS_SCRIPT;      // Scenar podle ktereho se ma chovat status registr - viz. cteni status registru

    uint8_t      waitForInt;      // Pokud jsme v rezimu INT, tak si zde radic pocita za jak dlouho
                                                // poslat dalsi interrupt - viz. FDC.waitForInt()

    uint8_t      write_track_stage;   // Pokud se formatuje, tak zde je ulozena uroven vychazejici z prijate znacky
                                                // 0 - zacatek zapisu stopy (prijimame data pro WRITE TRACK)
                                                // 1 - dorazila znacka indexu (0xfc)
                                                // 2 - dorazila znacka adresy (0xfe)
                                                // 3 - znacka dat sektoru (0xfb)
                                                // 4 - konec dat
                                                // 5 - konec stopy

    uint16_t      write_track_counter;   // Nuluje se vzdy pri zmene znacky, takze podle nej identifikujeme
                                                // kde se prave nachazime.

    uint8_t      reading_status_counter;   // pri cteni a zapisu sektoru pocitame kolikrat po sobe se
                                                // cetl regSTATUS bez toho, aniz by se mezi tim pracovalo s regDATA
                                                // pokud vice, nez 5x, tak se chovame jako kdyby uz byl konec sektoru.
                                                // Tohle cteni dat bez skutecneho cteni pouzivaji nektere programy jako
                                                // verifikaci ulozenych dat.
int fd0disabled;			//B.N. vysledek testu pritomnosti fd0disable.cfg
} FDC;

FDC *fdc_new(uint8_t port_base);

#endif
