// Host-side protocol harness for the Unicard-compatible device.
// Drives unicard.cpp through its port handlers exactly as the Z80 would and
// checks every status byte and data byte against the protocol as documented
// in mz800emu's unimgr.c (see docs/unicard-migration-plan.md).
#include "unicard.hpp"
#include "device.hpp"
#include "file.hpp"
#include "config.hpp"
#include "sharpmz_ascii.h"
#include "embedded_mzf.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <sys/stat.h>

FDCDevice* fdc = nullptr; QDDevice* qd = nullptr;
DEV_ENTRY devices[MAX_DEVICES]; uint8_t device_count = 0;
std::vector<std::pair<std::string, SectionConfig>> picoConfig;
std::string picoConfigPath;

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
    int nrec = 0; bool sawLong = false, sawDsk = false, sawDotDot = false;
    for (;;) { cmd(uc::cmdSTSR); if (!(st() & 0x04)) break; uint8_t rec[55]; for (int i = 0; i < 55; i++) rec[i] = rd(); nrec++;
        std::string lfn((char*)rec + 23, rec[22]); std::string sfn((char*)rec + 9);
        if (lfn == "LongFileName.mzf") { sawLong = true; CHECK(rec[0] == 300 - 256 && rec[1] == 1, "size 300 LE"); CHECK(sfn == "LONGFI~1.MZF", "8.3 alias '%s'", sfn.c_str()); }
        if (lfn == "a.dsk") { sawDsk = true; CHECK(rec[8] == AM_ARC, "attrib"); }
        if (lfn == "..") { sawDotDot = true; CHECK(rec[8] & AM_DIR, ".. is a dir"); }
    }
    cmd(uc::cmdSTSR);
    CHECK(nrec == 3 && sawLong && sawDsk && sawDotDot, "READDIR .. + 2 records (got %d)", nrec);
    st_is(0x00, 0x41, 0x00, 0x00, "READDIR closed at end");
    // NEXT skips a record; root listing includes subdirs with AM_DIR
    cmd(uc::cmdREADDIR); wstr("sd:/"); int total = 0; while (st() & 0x04) { cmd(uc::cmdNEXT); total++; }
    CHECK(total == 3, "root has 3 entries via NEXT (got %d)", total);
    cmd(uc::cmdNEXT); { uint8_t s[4]; st4(s); CHECK(s[0] & 0x80, "NEXT with no dir open -> ERROR"); }

    // --- 5. FILELIST text
    cmd(uc::cmdFILELIST); wstr("sd:/sub");
    { std::string dd = rstr(), dds = rstr(); CHECK(dd == "../" && dds == "0", "FILELIST .. line '%s' '%s'", dd.c_str(), dds.c_str()); }
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

    // --- 10. embedded pseudo-file, read exactly the way the Z80 loader does:
    //        128 header bytes, then the body length the header announces
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("@menu"); st_is(0x08, 0x50, 0x00, 0x00, "OPEN @menu");
    { uint8_t hdr[128]; for (int i = 0; i < 128; i++) hdr[i] = rd();
      CHECK(hdr[0] == 0x01 && hdr[1] == 'M' && hdr[18] == 0x2c && hdr[19] == 0x01, "@menu header via loader sequence");
      uint16_t body = hdr[18] | (hdr[19] << 8);
      int got = 0; for (int i = 0; i < body; i++) { rd(); got++; }
      CHECK(got == 300, "body streamed (%d)", got);
      CHECK(st() & 0x20, "EOF after exactly header+body");
      CHECK(rd() == 0x00, "read past end returns 0"); }
    cmd(uc::cmdCLOSE);
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("@menu");
    cmd(uc::cmdSIZE); { uint8_t a = rd(), b = rd(); rd(); rd(); CHECK((a | (b << 8)) == 428, "@menu SIZE 428"); }
    CHECK(rd() == 0x01 && rd() == 'M', "@menu first bytes after SIZE");
    cmd(uc::cmdSEEK); wr(1); wr(1); wr(0); wr(0); wr(0); rd(); CHECK(st() & 0x20, "@menu EOF after seek-from-end + read");
    cmd(uc::cmdOPEN); wr(FA_WRITE); wstr("@menu"); { uint8_t s[4]; st4(s); CHECK(s[0] & 0x80, "@menu not writable"); }
    cmd(uc::cmdCLOSE);

    // --- 10b. the full 42 KB @basic image, byte-exact through the loader sequence
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("@basic"); st_is(0x08, 0x50, 0x00, 0x00, "OPEN @basic");
    { uint8_t hdr[128]; for (int i = 0; i < 128; i++) hdr[i] = rd();
      uint32_t sz = hdr[18] | (hdr[19] << 8);
      CHECK(sz == sizeof(mzf_basic) - 128, "@basic header size %u matches the image", (unsigned)sz);
      bool ok = memcmp(hdr, mzf_basic, 128) == 0; size_t bad = 0;
      for (uint32_t i = 0; i < sz; i++) { uint8_t b = rd(); if (b != mzf_basic[128 + i]) { if (!bad) bad = 128 + i + 1; ok = false; } }
      CHECK(ok, "@basic body byte-exact (first mismatch at %zu)", bad);
      CHECK(st() & 0x20, "@basic EOF after exactly header+body"); }
    cmd(uc::cmdCLOSE); st_is(0x00, 0x54, 0x00, 0x00, "CLOSE @basic");
    // SERVEDSUM: 16-bit sum of everything served since OPEN (loader verification)
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("@menu");
    { uint16_t sum = 0; for (size_t i = 0; i < sizeof(mzf_menu); i++) sum += rd();
      cmd(uc::cmdX_SERVEDSUM); uint8_t s[4]; st4(s); CHECK((s[0] & 0x02) && s[2] == 2, "SERVEDSUM output 2 bytes (%02x %02x)", s[0], s[2]);
      uint16_t got = rd(); got |= rd() << 8; CHECK(got == sum, "SERVEDSUM %04x == %04x", got, sum); }
    cmd(uc::cmdCLOSE);

    // --- 11. FDDMOUNT drive 1 and eject, QD via id 5
    cmd(uc::cmdFDDMOUNT); wr(1); wstr("sd:/games/a.dsk"); st_is(0x00, 0x10, 0x00, 0x00, "FDDMOUNT");
    CHECK(fdcdev.mounted[1] == "sd:/games/a.dsk", "drive 2 mounted");
    cmd(uc::cmdFDDMOUNT); wr(1); wr(0x0D); CHECK(fdcdev.mounted[1].empty(), "drive 2 ejected");
    cmd(uc::cmdFDDMOUNT); wr(5); wstr("sd:/x.mzq"); CHECK(qddev.path == "sd:/x.mzq", "QD mounted via id 5");
    cmd(uc::cmdFDDMOUNT); wr(2); wstr("sd:/games/a.dsk");
    cmd(uc::cmdX_MOUNTS); { std::string m; for (int i = 0; i < 5; i++) { m += rstr(); m += "|"; } CHECK(m == "1:|2:|3:sd:/games/a.dsk|4:|Q:sd:/x.mzq|", "MOUNTS text '%s'", m.c_str()); }
    cmd(uc::cmdFDDMOUNT); wr(2); wr(0x0D);

    // --- SETCONFIG: in-memory config and the loaded ini rewritten in place
    picoConfigPath = "sd:/mzpico.ini";
    writefile(root + "/mzpico.ini", "; header\r\n[menu]\r\nkey_b=Basic|@basic\r\n\r\n[fdc]\r\nimage_disk1=x\r\n");
    auto slurp = [&]() { std::ifstream f(root + "/mzpico.ini", std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); };
    cmd(uc::cmdX_SETCONFIG); wstr("menu"); wstr("key_x"); wstr("Test|sd:/t.mzf"); st_is(0x00, 0x99, 0x00, 0x00, "SETCONFIG add");
    CHECK(slurp() == "; header\r\n[menu]\r\nkey_b=Basic|@basic\r\nkey_x=Test|sd:/t.mzf\r\n\r\n[fdc]\r\nimage_disk1=x\r\n", "ini after add: '%s'", slurp().c_str());
    cmd(uc::cmdX_GETCONFIG); wstr("menu"); { int n = 0; while (st() & 0x04) { for (int i = 0; i < 80; i++) rd(); n++; } CHECK(n == 3, "GETCONFIG sees 3 menu keys (%d)", n); }
    cmd(uc::cmdX_SETCONFIG); wstr("menu"); wstr("key_b"); wstr("Basic2|@basic"); st_is(0x00, 0x99, 0x00, 0x00, "SETCONFIG replace");
    CHECK(slurp().find("key_b=Basic2|@basic\r\nkey_x=") != std::string::npos, "ini after replace: '%s'", slurp().c_str());
    cmd(uc::cmdX_SETCONFIG); wstr("menu"); wstr("key_x"); wr(0x0D); st_is(0x00, 0x99, 0x00, 0x00, "SETCONFIG delete");
    CHECK(slurp() == "; header\r\n[menu]\r\nkey_b=Basic2|@basic\r\n\r\n[fdc]\r\nimage_disk1=x\r\n", "ini after delete: '%s'", slurp().c_str());
    cmd(uc::cmdX_SETCONFIG); wstr("explorer"); wstr("start"); wstr("sd:/games"); st_is(0x00, 0x99, 0x00, 0x00, "SETCONFIG new section");
    CHECK(slurp().find("[explorer]\r\nstart=sd:/games\r\n") != std::string::npos, "ini new section: '%s'", slurp().c_str());
    cmd(uc::cmdX_SETCONFIG); wstr("menu"); wstr("key_b"); wstr("Basic|@basic");   // restore for the tests below
    cmd(uc::cmdFDDMOUNT); wr(9); wstr("x"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[2] == uc::errBAD_PARAM, "bad device id"); }

    // --- 12. extensions
    cmd(uc::cmdX_LISTVOL); { std::string a = rstr(), b = rstr(); CHECK(a == "sd:" && b == "flash:", "LISTVOL '%s' '%s'", a.c_str(), b.c_str()); }
    cmd(uc::cmdX_GETCONFIG); wstr("menu"); { uint8_t s[4]; st4(s); CHECK(s[0] == 0x04 && s[2] == 80, "GETCONFIG record stream"); uint8_t rec[80]; for (int i = 0; i < 80; i++) rec[i] = rd(); CHECK(!strcmp((char*)rec, "key_b") && !strcmp((char*)rec + 16, "Basic|@basic"), "config record 1"); for (int i = 0; i < 80; i++) rd(); CHECK((st() & 0x04) == 0, "config stream ended"); }
    cmd(uc::cmdX_INFO); { uint8_t s[4]; st4(s); CHECK(s[2] == 16, "INFO 16 bytes"); for (int i = 0; i < 16; i++) rd(); }
    cmd(0x77); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[1] == 0x77 && s[2] == uc::errNOT_IMPLEMENTED, "unknown command"); }

    // --- 12b. SETSORT: explorer listing = "..", dirs, then files sorted, non-launchable dropped
    writefile(root + "/games/readme.txt", "no"); writefile(root + "/games/Zeta.mzf", "z"); writefile(root + "/games/beta.DSK", "b");
    system(("mkdir -p " + root + "/games/sub2 " + root + "/games/Alpha").c_str());
    cmd(uc::cmdX_SETSORT); wr(0x02); st_is(0x00, 0x96, 0x00, 0x00, "SETSORT (launchable filter)");
    cmd(uc::cmdREADDIR); wstr("sd:/games");
    { std::vector<std::string> names; for (;;) { cmd(uc::cmdSTSR); if (!(st() & 0x04)) break; uint8_t rec[55]; for (int i = 0; i < 55; i++) rec[i] = rd(); names.push_back(std::string((char*)rec + 23, rec[22])); }
      std::sort(names.begin(), names.end());
      std::vector<std::string> want = {"..", "Alpha", "LongFileName.mzf", "Zeta.mzf", "a.dsk", "beta.DSK", "sub2"};
      std::sort(want.begin(), want.end());
      CHECK(names == want, "filtered listing has %zu entries: %s", names.size(), [&]{ std::string o; for (auto& n : names) o += n + " "; return o; }().c_str()); }
    cmd(uc::cmdREADDIR); wstr("sd:/"); { int n = 0; bool dotdot = false; for (;;) { cmd(uc::cmdSTSR); if (!(st() & 0x04)) break; uint8_t rec[55]; for (int i = 0; i < 55; i++) rec[i] = rd(); if (std::string((char*)rec + 23, rec[22]) == "..") dotdot = true; n++; } CHECK(!dotdot && n == 2, "root listing has no .. and is filtered (n=%d)", n); }
    cmd(uc::cmdX_SETSORT); wr(0x00);
    cmd(uc::cmdREADDIR); wstr("sd:/games"); { int n = 0; for (;;) { cmd(uc::cmdSTSR); if (!(st() & 0x04)) break; cmd(uc::cmdNEXT); n++; } CHECK(n == 8, "unfiltered listing has .. + 7 (n=%d)", n); }

    // --- 12c. cloud paths without WiFi support: invalid drive, no state change
    cmd(uc::cmdREADDIR); wstr("cloud:/"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[3] == FR_INVALID_DRIVE && !(s[0] & 0x40), "cloud READDIR without W"); }
    cmd(uc::cmdOPEN); wr(FA_READ); wstr("cloud:/x.mzf"); { uint8_t s[4]; st4(s); CHECK((s[0] & 0x80) && s[3] == FR_INVALID_DRIVE, "cloud OPEN without W"); }

    // --- 13. RESET closes everything and returns to the root
    cmd(uc::cmdCHDIR); wstr("games"); cmd(uc::cmdOPEN); wr(FA_READ); wstr("a.dsk"); cmd(uc::cmdRESET);
    st_is(0x00, 0x00, 0x00, 0x00, "RESET status"); cmd(uc::cmdGETCWD); CHECK(rstr() == "sd:/", "RESET returns to root");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
