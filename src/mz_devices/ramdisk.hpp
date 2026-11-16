#pragma once

#include <stdint.h>
#include "byte_source.hpp"
#include "mz_devices.hpp"
#include "common.hpp"
#include "cow_store.hpp"
#include "cow_image.hpp"

constexpr uint32_t RAMDISK_DEFAULT_SIZE = 65536;
constexpr uint8_t RAMDISK_DEFAULT_BASE_PORT = 0xe9;
constexpr const char RAMDISK_ID[] = "ramdisk";
constexpr bool RAMDISK_EXWAIT = true;
constexpr uint32_t RAMDISK_COW_DEFAULT_POOL_KB = 64;

// REST / diagnostics (core 0): the COW RAM disk's state as a JSON object,
// and a pool clear armed for the next Z80 reset. Plain C linkage so the C
// heap diagnostic can embed the object.
extern "C" int ramdisk_cow_json(char* buf, size_t len);        // brief (inside /api/heap)
extern "C" int ramdisk_cow_json_full(char* buf, size_t len);   // GET /api/ramdisk
extern "C" void ramdisk_cow_request_clear(void);
// learn: "relearn" | "freeze" | "unfreeze", applied at the next Z80 reset;
// -1 when there is no learn disk
extern "C" int ramdisk_cow_request(const char* what);

class RamDisk final : public MZDevice {
public:
    RamDisk();
    int init() override;
    int isInterrupt() override { return 0; };
    bool needsExwait() const override { return RAMDISK_EXWAIT; }
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t basePort) const override;
    int readConfig(dictionary *ini) override;
    int flush() override;
    static std::string getDevType() { return RAMDISK_ID; }

    RAM_FUNC static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int resetCounter(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writePageAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readHighAddr(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);

    // Contents persist, like real RAM (in COW mode: the pool survives
    // every Z80 reset; only a card power-up - or a reboot - clears it)
    void softReset() override;

    // mode=cow: the store (state for REST), and flash_fs's page-move hook
    const CowStore& cowStore() const { return cow; }
    const CowImage& cowImageRef() const { return cowImage; }
    bool persistDue() const { return persist_due_; }
    void cowRemap(uint16_t old_entry, uint16_t new_entry) { cowImage.onRemap(cow, old_entry, new_entry); }
    void cowTrim(uint32_t first, uint32_t count) { cowImage.onTrim(cow, first, count); }

    // Deluxe only: 16-bit intra-page positioning (what real MZ-1R18
    // software uses) needs the Deluxe write capture - on Frugal the
    // device would silently corrupt random-access writes, so it is
    // skipped entirely. NB: the frugal bus-validation instruments
    // (rdtest/rdrtest/rdstress) target these ports; a frugal bus
    // campaign needs a diagnostic build with this returning true.
    bool supportedOnBoard() const override {
        #ifdef BOARD_DELUXE
        return true;
        #else
        return false;
        #endif
    }

private:
    int readConfigCow(dictionary* ini, const std::string& image, uint32_t size_given);

    bool cow_ = false;      // mode=cow: flash image (XIP) + SRAM copy-on-write pool
    CowStore cow;
    CowImage cowImage;
    CowLearnPolicy learnPolicy_;
    bool active_ = false;   // the Z80 addressed the disk this session (learn's convergence)
    bool persist_due_ = false;  // learn: trailer/map sync deferred from the reset
    void cowPersist();
    uint8_t* data;
    bool readOnly;
    uint32_t size;
    uint32_t pos_;
    std::unique_ptr<ByteSource> bs;
};
