// Host-side protocol harness for the Unicard-compatible device.
// Drives unicard.cpp through its port handlers exactly as the Z80 would and
// checks every status byte and data byte against the protocol as documented
// in mz800emu's unimgr.c (see docs/unicard-migration-plan.md).
#include "unicard.hpp"
#include "device.hpp"
#include "file.hpp"
#include "config.hpp"
#include "sharpmz_ascii.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>

FDCDevice* fdc = nullptr; QDDevice* qd = nullptr;
DEV_ENTRY devices[MAX_DEVICES]; uint8_t device_count = 0;
std::vector<std::pair<std::string, SectionConfig>> picoConfig;

static UnicardDevice* dev;
static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void cmd(uint8_t c) { dev->getWriteMappings()[0].fn(dev, 0x50, c, 0); }
static void wr(uint8_t b) { dev->getWriteMappings()[1].fn(dev, 0x51, b, 0); }
static uint8_t rd() { uint8_t v = 0xEE; dev->getReadMappings()[1].fn(dev, 0x51, &v, 0); return v; }
static uint8_t st() { uint8_t v = 0xEE; dev->getReadMappings()[0].fn(dev, 0x50, &v, 0); return v; }
static void st4(uint8_t s[4]) { for (int i = 0; i < 4; i++) s[i] = st(); }
static void wstr(const char* s) { while (*s) wr((uint8_t)*s++); wr(0x0D); }
static std::string rstr() { std::string o; for (int i = 0; i < 300; i++) { uint8_t c = rd(); if (c == 0x0D) break; o += (char)c; } return o; }
static bool st_is(uint8_t a, uint8_t b, uint8_t c, uint8_t d, const char* what) {
    uint8_t s[4]; st4(s);
    bool ok = s[0] == a && s[1] == b && s[2] == c && s[3] == d;
    CHECK(ok, "%s: status %02x %02x %02x %02x, expected %02x %02x %02x %02x", what, s[0], s[1], s[2], s[3], a, b, c, d);
    return ok;
}
static void writefile(const std::string& p, const std::string& c) { std::ofstream f(p, std::ios::binary); f << c; }

int main() {
    std::string root = "/tmp/unicard_sim_root"; std::string fl = "/tmp/unicard_sim_flash";
    system(("rm -rf " + root + " " + fl + " && mkdir -p " + root + "/games " + root + "/sub/deep " + fl).c_str());
    writefile(root + "/hello.txt", "Hello, MZ-800!\n");
    writefile(root + "/games/LongFileName.mzf", std::string(300, 'x'));
    writefile(root + "/games/a.dsk", "dsk");
    writefile(fl + "/f.txt", "flash");
    ffstub_init(root.c_str(), fl.c_str());
    strcpy(devices[0].name, "sd"); strcpy(devices[1].name, "flash"); device_count = 2;
    picoConfig.push_back({"menu", {{"key_b", "Basic|@basic"}, {"key_e", "Explorer|@explorer"}}});
    FDCDevice fdcdev; QDDevice qddev; fdc = &fdcdev; qd = &qddev;

    dev = new UnicardDevice();
    dev->init();
    dev->readConfig(nullptr);

    // --- 1. detection: REVD as MZIX / uc.c do it
    cmd(uc::cmdSTORNO); cmd(uc::cmdREVD);
    st_is(0x02, 0x06, 0x04, 0x00, "REVD status");
    uint8_t r[4]; for (int i = 0; i < 4; i++) r[i] = rd();
    CHECK(r[2] == 0x4D, "REVD subtype 'M' (got %02x)", r[2]);
    st_is(0x00, 0x06, 0x00, 0x00, "after REVD output drained");
    // status pointer parks after 4 bytes, STSR rewinds, data access rewinds
    CHECK(st() == 0x00 && st() == 0x00, "parked status reads 0");
    cmd(uc::cmdSTSR); uint8_t s0 = st(); CHECK(s0 == 0x00, "STSR rewound (byte0=%02x)", s0);
    CHECK(st() == 0x06, "STSR keeps last cmd (REVD)");

    // --- 2. REV text, 0x0D terminated
    cmd(uc::cmdREV); { uint8_t s[4]; st4(s); CHECK(s[0] == 0x02 && s[1] == 0x05, "REV status %02x %02x", s[0], s[1]); }
    std::string rev = rstr(); CHECK(rev.rfind("MZPico", 0) == 0, "REV text '%s'", rev.c_str());
    st_is(0x00, 0x05, 0x00, 0x00, "after REV");

    // --- 3. GETCWD / CHDIR relative and absolute, volume switch
    cmd(uc::cmdGETCWD); CHECK(rstr() == "sd:/", "cwd is sd root");
    cmd(uc::cmdCHDIR); st_is(0x01, 0x21, 254, 0x00, "CHDIR waiting (BUSY, 254 free)"); wstr("games");
    st_is(0x00, 0x21, 0x00, 0x00, "CHDIR done");
    cmd(uc::cmdGETCWD); CHECK(rstr() == "sd:/games", "cwd after relative chdir");
    cmd(uc::cmdCHDIR); wstr(".."); cmd(uc::cmdGETCWD); CHECK(rstr() == "sd:/", "cwd after ..");
    cmd(uc::cmdCHDIR); wstr("flash:/"); cmd(uc::cmdGETCWD); CHECK(rstr() == "flash:/", "volume switch");
    cmd(uc::cmdCHDIR); wstr("nonexistent"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[2] == uc::errFATFS && s[3] == FR_NO_PATH, "CHDIR error %02x %02x %02x %02x", s[0], s[1], s[2], s[3]); }
    cmd(uc::cmdCHDIR); wstr("sd:/");

    // --- 4. READDIR binary records
    cmd(uc::cmdREADDIR); wstr("sd:/games");
    { uint8_t s[4]; st4(s); CHECK(s[0] == 0x04 && s[1] == 0x41 && s[2] == 55, "READDIR first record status %02x %02x %02x", s[0], s[1], s[2]); }
    int nrec = 0; bool sawLong = false, sawDsk = false;
    for (;;) { cmd(uc::cmdSTSR); if (!(st() & 0x04)) break; uint8_t rec[55]; for (int i = 0; i < 55; i++) rec[i] = rd(); nrec++;
        std::string lfn((char*)rec + 23, rec[22]); std::string sfn((char*)rec + 9);
        if (lfn == "LongFileName.mzf") { sawLong = true; CHECK(rec[0] == 300 - 256 && rec[1] == 1, "size 300 LE"); CHECK(sfn == "LONGFI~1.MZF", "8.3 alias '%s'", sfn.c_str()); }
        if (lfn == "a.dsk") { sawDsk = true; CHECK(rec[8] == AM_ARC, "attrib"); }
    }
    cmd(uc::cmdSTSR);
    CHECK(nrec == 2 && sawLong && sawDsk, "READDIR 2 records (got %d)", nrec);
    st_is(0x00, 0x41, 0x00, 0x00, "READDIR closed at end");
    // NEXT skips a record; root listing includes subdirs with AM_DIR
    cmd(uc::cmdREADDIR); wstr("sd:/"); int total = 0; while (st() & 0x04) { cmd(uc::cmdNEXT); total++; }
    CHECK(total == 3, "root has 3 entries via NEXT (got %d)", total);
    cmd(uc::cmdNEXT); { uint8_t s[4]; st4(s); CHECK(s[0] & 0x80, "NEXT with no dir open -> ERROR"); }

    // --- 5. FILELIST text
    cmd(uc::cmdFILELIST); wstr("sd:/sub");
    { std::string name = rstr(), size = rstr(); CHECK(name == "deep/" && size == "0", "FILELIST dir line '%s' '%s'", name.c_str(), size.c_str()); }
    CHECK((st() & 0x04) == 0, "FILELIST ended");

    // --- 6. OPEN / getc / EOF / TELL / SIZE / SEEK
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("hello.txt");
    st_is(0x08, 0x50, 0x00, 0x00, "OPEN read ok (READ_FILE)");
    cmd(uc::cmdSIZE); { uint8_t s[4]; st4(s); CHECK(s[0] == 0x0A && s[1] == 0x56 && s[2] == 4, "SIZE status %02x %02x %02x", s[0], s[1], s[2]); }
    CHECK(rd() == 15 && rd() == 0 && rd() == 0 && rd() == 0, "SIZE = 15");
    std::string text; for (int i = 0; i < 15; i++) text += (char)rd();
    CHECK(text == "Hello, MZ-800!\n", "getc stream '%s'", text.c_str());
    st_is(0x28, 0xF0, 0x00, 0x00, "EOF bit after last byte, last cmd INTGETC");
    CHECK(rd() == 0x00, "read past EOF returns 0");
    cmd(uc::cmdSEEK); wr(0); wr(7); wr(0); wr(0); wr(0); st_is(0x08, 0x51, 0x00, 0x00, "SEEK set");
    CHECK(rd() == 'M', "byte at 7");
    cmd(uc::cmdSEEK); wr(1); wr(2); wr(0); wr(0); wr(0); CHECK(rd() == '!', "SEEK from end 2");
    cmd(uc::cmdSEEK); wr(3); wr(3); wr(0); wr(0); wr(0); cmd(uc::cmdTELL); rd(); rd(); rd(); rd();
    cmd(uc::cmdSEEK); wr(2); wr(1); wr(0); wr(0); wr(0); cmd(uc::cmdTELL); { uint8_t t = rd(); rd(); rd(); rd(); CHECK(t == 12, "TELL after rel seeks = 12 (got %d)", t); }
    // a command with output takes priority over getc, then getc resumes
    cmd(uc::cmdREVD); rd(); rd(); rd(); rd(); CHECK(rd() == '0', "getc resumes after REVD output (pos 12)");
    cmd(uc::cmdCLOSE); st_is(0x00, 0x54, 0x00, 0x00, "CLOSE");

    // --- 7. write path: create, putc, SYNC, TRUNC, CLOSE, verify; STAT
    cmd(uc::cmdOPEN); wr(FA_WRITE | FA_CREATE_ALWAYS); wstr("sd:/out.txt");
    st_is(0x30, 0x50, 0x00, 0x00, "OPEN write (WRITE_FILE, empty file is at EOF)");
    for (char c : std::string("abcdef")) wr((uint8_t)c);
    st_is(0x30, 0xF1, 0x00, 0x00, "after putc: WRITE_FILE|EOF, INTPUTC");
    cmd(uc::cmdSEEK); wr(0); wr(4); wr(0); wr(0); wr(0); cmd(uc::cmdTRUNC); st_is(0x30, 0x52, 0x00, 0x00, "TRUNC at 4");
    cmd(uc::cmdCLOSE);
    { std::ifstream f(root + "/out.txt"); std::string c((std::istreambuf_iterator<char>(f)), {}); CHECK(c == "abcd", "file content '%s'", c.c_str()); }
    cmd(uc::cmdSTAT); wstr("out.txt"); { uint8_t s[4]; st4(s); CHECK(s[0] == 0x02 && s[2] == 55, "STAT output 55 bytes"); uint8_t rec[55]; for (int i = 0; i < 55; i++) rec[i] = rd(); CHECK(rec[0] == 4 && std::string((char*)rec + 23, rec[22]) == "out.txt", "STAT record"); }
    cmd(uc::cmdRENAME); wstr("out.txt"); wstr("sub/moved.txt"); st_is(0x00, 0x34, 0x00, 0x00, "RENAME");
    cmd(uc::cmdMKDIR); wstr("newdir"); st_is(0x00, 0x40, 0x00, 0x00, "MKDIR");
    cmd(uc::cmdUNLINK); wstr("newdir"); st_is(0x00, 0x31, 0x00, 0x00, "UNLINK dir");
    cmd(uc::cmdUNLINK); wstr("sub/moved.txt"); st_is(0x00, 0x31, 0x00, 0x00, "UNLINK file");
    cmd(uc::cmdUNLINK); wstr("gone"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[3] == FR_NO_FILE, "UNLINK missing -> FatFS code"); }
    cmd(uc::cmdGETFREE); { uint8_t s[4]; st4(s); CHECK(s[0] == 0x02 && s[2] == 8, "GETFREE 8 bytes"); for (int i = 0; i < 8; i++) rd(); }
    cmd(uc::cmdCHMOD); wr(1); wr(1); wstr("hello.txt"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[2] == uc::errNOT_IMPLEMENTED, "CHMOD not implemented code"); }

    // --- 8. STORNO mid-parameters; overflow
    cmd(uc::cmdCHDIR); wr('g'); wr('a'); cmd(uc::cmdSTORNO); st_is(0x00, 0x04, 0x00, 0x00, "STORNO cancels BUSY");
    cmd(uc::cmdGETCWD); CHECK(rstr() == "sd:/", "cwd unchanged after STORNO");
    cmd(uc::cmdMKDIR); for (int i = 0; i < 254; i++) wr('a'); // 254 = the string buffer's capacity { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[2] == uc::errOVERFLOW && !(s[0] & 0x01), "overflow -> ERROR, not BUSY (%02x %02x)", s[0], s[2]); }

    // --- 9. SHASCII: a lowercase name round-trips through the translator
    cmd(uc::cmdSHASCII); cmd(uc::cmdOPEN); wr(FA_READ); { const char* n = "hello.txt"; while (*n) wr(sharpmz_cnv_to((uint8_t)*n++)); wr(0x0D); }
    st_is(0x08, 0x50, 0x00, 0x00, "OPEN with SharpASCII name");
    cmd(uc::cmdCLOSE); cmd(uc::cmdGETCWD); { std::string c; for (int i = 0; i < 40; i++) { uint8_t b = rd(); if (b == 0x0D) break; c += (char)sharpmz_cnv_from(b); } CHECK(c == "sd:/", "GETCWD converted back '%s'", c.c_str()); }
    cmd(uc::cmdASCII);

    // --- 10. embedded pseudo-file
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("@menu"); st_is(0x08, 0x50, 0x00, 0x00, "OPEN @menu");
    cmd(uc::cmdSIZE); rd(); rd(); rd(); rd(); CHECK(rd() == 0x01 && rd() == 'M', "@menu bytes");
    cmd(uc::cmdSEEK); wr(1); wr(1); wr(0); wr(0); wr(0); CHECK(rd() == 0xCC, "@menu seek from end"); CHECK(st() & 0x20, "@menu EOF");
    cmd(uc::cmdOPEN); wr(FA_WRITE); wstr("@menu"); { uint8_t s[4]; st4(s); CHECK(s[0] & 0x80, "@menu not writable"); }
    cmd(uc::cmdCLOSE);

    // --- 11. FDDMOUNT drive 1 and eject, QD via id 5
    cmd(uc::cmdFDDMOUNT); wr(1); wstr("sd:/games/a.dsk"); st_is(0x00, 0x10, 0x00, 0x00, "FDDMOUNT");
    CHECK(fdcdev.mounted[1] == "sd:/games/a.dsk", "drive 2 mounted");
    cmd(uc::cmdFDDMOUNT); wr(1); wr(0x0D); CHECK(fdcdev.mounted[1].empty(), "drive 2 ejected");
    cmd(uc::cmdFDDMOUNT); wr(5); wstr("sd:/x.mzq"); CHECK(qddev.path == "sd:/x.mzq", "QD mounted via id 5");
    cmd(uc::cmdFDDMOUNT); wr(9); wstr("x"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[2] == uc::errBAD_PARAM, "bad device id"); }

    // --- 12. extensions
    cmd(uc::cmdX_LISTVOL); { std::string a = rstr(), b = rstr(); CHECK(a == "sd:" && b == "flash:", "LISTVOL '%s' '%s'", a.c_str(), b.c_str()); }
    cmd(uc::cmdX_GETCONFIG); wstr("menu"); { uint8_t s[4]; st4(s); CHECK(s[0] == 0x04 && s[2] == 80, "GETCONFIG record stream"); uint8_t rec[80]; for (int i = 0; i < 80; i++) rec[i] = rd(); CHECK(!strcmp((char*)rec, "key_b") && !strcmp((char*)rec + 16, "Basic|@basic"), "config record 1"); for (int i = 0; i < 80; i++) rd(); CHECK((st() & 0x04) == 0, "config stream ended"); }
    cmd(uc::cmdX_INFO); { uint8_t s[4]; st4(s); CHECK(s[2] == 16, "INFO 16 bytes"); for (int i = 0; i < 16; i++) rd(); }
    cmd(0x77); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[1] == 0x77 && s[2] == uc::errNOT_IMPLEMENTED, "unknown command"); }

    // --- 13. RESET closes everything and returns to the root
    cmd(uc::cmdCHDIR); wstr("games"); cmd(uc::cmdOPEN); wr(FA_READ); wstr("a.dsk"); cmd(uc::cmdRESET);
    st_is(0x00, 0x00, 0x00, 0x00, "RESET status"); cmd(uc::cmdGETCWD); CHECK(rstr() == "sd:/", "RESET returns to root");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
