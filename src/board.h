// myboard.h

// setting first overrides the value in the default header

#ifdef MZPICO_THIRD_PARTY_16M
// 16 MB modules are by definition third-party boards (genuine Picos ship
// 2 MB Winbond): halve the QSPI clock (90 -> 45 MHz at the 180 MHz
// sysclock) for zbit/XTX-class clone flash. Costs ~10 ms at boot (bigger
// map, slower reads) - budgeted for in the power-up boot race, see the
// cold-boot note in device_main().
#define PICO_FLASH_SPI_CLKDIV 4
#else
#define PICO_FLASH_SPI_CLKDIV 2     // Winbond flash on genuine boards is fine at 90 MHz
#endif

// pick up the rest of the settings
#ifdef USE_PICO_W
#include "boards/pico_w.h"
#else
#include "boards/pico.h"
#endif
