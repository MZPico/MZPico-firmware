/* Build-time guard: the firmware image must end below the flash
 * filesystem, which starts at MZPICO_FS_BASE (set per flash size in
 * CMakeLists.txt and passed here with --defsym; src/flash_fs.c uses the
 * same value). Linked as an extra input script next to the SDK's memmap;
 * the ASSERT runs after layout.
 *
 * Why it exists: the v0.5.0 Pico W release images (CI, GCC 14) ran 11-22 KB
 * past the then 1 MB boundary. Flashing one overwrote the filesystem's map
 * sectors, the volume no longer mounted (and, being non-blank, was never
 * auto-formatted) and the USB drive vanished.
 */
ASSERT(__flash_binary_end <= 0x10000000 + MZPICO_FS_BASE,
       "MZPico: firmware image too large - it would overwrite the flash filesystem (MZPICO_FS_BASE)")
