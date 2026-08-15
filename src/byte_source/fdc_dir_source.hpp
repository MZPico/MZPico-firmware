#pragma once
#include <cstdint>
#include <string>
#include <memory>

#include "ff.h"
#include "cached_source.hpp"

// Directory-mounted floppy: presents a directory of files as a synthesized
// extended-DSK image to the FDC device, with one of two on-disk filesystem
// personalities:
//
//   BASIC - Sharp MZ-800 Disk BASIC (MZ-2Z046 "FSMZ"): 40 cyl x 2 sides x
//           16 x 256 B = 320 KB, all media bytes bit-inverted, directory of
//           63 entries served from the .mzf files in the directory.
//   CPM   - LEC CP/M 2.2 (Lamac) SD: 80 cyl x 2 sides x 9 x 512 B = 720 KB
//           (DSK track 1 is the 16 x 256 Sharp boot track), non-inverted
//           data, 128 directory entries, 2 KB blocks, 4 reserved tracks.
//
// Reads are synthesized on demand (metadata from a RAM model, file data
// straight from FatFS). Writes work through a staging engine: data written
// to unallocated blocks is staged in a temp file until the guest rewrites
// the on-disk directory, which reveals file identities - the commit engine
// then diffs the directory against the model and materializes/deletes/
// renames the FAT files. After a commit the guest's directory image IS the
// served truth (the disk must not shift under the guest's cached allocation
// state - Disk BASIC literally errors "Disk mismatch" if it does).
//
// Formatting (WRITE TRACK) flows through the same path: the format fill
// wipes the directory sectors, and the commit engine deletes all files -
// the same policy as QD directory mounts. Boot tracks are not stored:
// directory-mounted disks are not bootable.
class FDCDirSource : public CachedSource {
public:
    enum class Fs : std::uint8_t { BASIC, CPM };

    FDCDirSource(const std::string& path, Fs fs, std::uint32_t cache_size);
    ~FDCDirSource();

    bool readOnly() const override { return false; }
    int flush() override;                       // cache flush + commit engine
    int resize(std::uint32_t) override { return 0; } // fixed geometry: accept & ignore
    bool valid() const { return valid_; }
    Fs fsType() const { return fs_; }

    // Z80 soft reset: abandon in-flight guest state (staged blocks of an
    // unfinished save, format tracking); the mounted model itself persists
    void sessionAbort();

    // True (and clears) if any guest write was dropped or a commit write
    // failed since the last call - the FDC turns this into a write fault
    bool takeWriteError() {
        const bool e = write_error_;
        write_error_ = false;
        return e;
    }

    // Heuristic for ini "auto": a directory holding .mzf files is a BASIC
    // disk, anything else is CP/M
    static Fs detectFs(const std::string& path);

private:
    // ---- geometry ----------------------------------------------------------
    // BASIC: 80 DSK track records of 0x100 Track-Info + 16*256 data
    static constexpr std::uint32_t kBasTracks     = 80;
    static constexpr std::uint32_t kBasTrackRec   = 0x1100;
    static constexpr std::uint32_t kBasTotalSize  = 0x100 + kBasTracks * kBasTrackRec;
    static constexpr std::uint16_t kBasBlocks     = 1280; // 256-byte alloc blocks
    static constexpr std::uint16_t kBasFarea      = 48;   // first file-area block
    static constexpr std::uint16_t kBasDinfoBlock = 15;
    static constexpr std::uint16_t kBasDirBlock   = 16;   // blocks 16..23
    static constexpr std::uint32_t kBasDirBytes   = 2048; // 64 entries * 32 B
    static constexpr std::uint8_t  kBasMaxFiles   = 63;   // slot 0 is the 0x80 marker

    // CP/M (LEC SD): DSK track 0 = 9*512, track 1 = 16*256 (Sharp boot
    // track), tracks 2..159 = 9*512
    static constexpr std::uint32_t kCpmTracks     = 160;
    static constexpr std::uint32_t kCpmTrackRec   = 0x1300; // 9*512 + header
    static constexpr std::uint32_t kCpmBootRec    = 0x1100; // 16*256 + header
    static constexpr std::uint32_t kCpmTotalSize  =
        0x100 + kCpmBootRec + (kCpmTracks - 1) * kCpmTrackRec;
    static constexpr std::uint16_t kCpmOffTracks  = 4;    // reserved tracks
    static constexpr std::uint16_t kCpmDsm        = 350;  // highest block number
    static constexpr std::uint32_t kCpmBlockSize  = 2048;
    static constexpr std::uint16_t kCpmDirRecs    = 32;   // 4 KB = 128 entries
    static constexpr std::uint32_t kCpmDirBytes   = 4096;
    static constexpr std::uint8_t  kCpmMaxEnts    = 128;

    // ---- staging -----------------------------------------------------------
    // Guest-sector granularity (256 B BASIC / 512 B CP/M); generously sized
    // for the largest single commit (a full 64 KB BASIC save = 256 sectors)
    static constexpr std::uint16_t kMaxStage      = 288;
    static constexpr std::uint16_t kStageNone     = 0xFFFF;
    struct StageEnt {
        std::uint16_t key;  // BASIC: block number; CPM: data-area 512B sector index
        std::uint16_t slot; // slot index in the staging temp file
    };

    struct OpenFile {
        FIL         f{};
        std::string filename{};
        bool        open{false};
        bool        writable{false};
    };

    // ---- CachedSource callbacks -------------------------------------------
    static int fetch(void* ctx, std::uint32_t index,
                     std::uint8_t* buf, std::uint32_t size, std::uint32_t& read);
    static int store(void* ctx, std::uint32_t index, const std::uint8_t* data,
                     std::uint32_t size, std::uint32_t& written);
    int fetch_bytes(std::uint32_t index, std::uint8_t* out,
                    std::uint32_t size, std::uint32_t& read);
    int store_bytes(std::uint32_t index, const std::uint8_t* data,
                    std::uint32_t size);

    // ---- DSK container arithmetic -----------------------------------------
    std::uint32_t totalSize() const;
    // Decode a DSK offset: returns the track index and the offset within its
    // record; track == -1 means the 0x100 disk header
    void locate(std::uint32_t index, int& track, std::uint32_t& in_track) const;
    std::uint32_t trackRecSize(int track) const;
    std::uint16_t trackSectors(int track) const;  // sector count
    std::uint16_t trackSecSize(int track) const;  // bytes per sector
    std::uint8_t  headerByte(std::uint32_t off) const;
    std::uint8_t  tinfoByte(int track, std::uint32_t off) const;

    // ---- model build -------------------------------------------------------
    void buildBasic();
    void buildCpm();
    const char* fullPath(const std::string& filename);

    // ---- serving -----------------------------------------------------------
    // Serve one span of sector data; returns bytes emitted (>0) into out
    std::uint32_t serveData(int track, std::uint16_t sec, std::uint32_t k,
                            std::uint8_t* out, std::uint32_t maxlen);
    // Map a BASIC block to (dir slot, byte offset in body); -1 if unmapped
    int basMapBlock(std::uint16_t block, std::uint32_t& body_off) const;
    // Map a CP/M block to (file table index, byte offset); -1 if unmapped
    int cpmMapBlock(std::uint16_t block, std::uint32_t& file_off) const;

    // ---- staging -----------------------------------------------------------
    std::uint16_t stageSecSize() const { return fs_ == Fs::BASIC ? 256 : 512; }
    int  stageFind(std::uint16_t key) const;
    // Overlay [off, off+len) of the staged guest sector; creates the slot
    // (prefilled with fill) on first touch. Returns 0 on success.
    int  stagePut(std::uint16_t key, std::uint32_t off,
                  const std::uint8_t* data, std::uint32_t len, std::uint8_t fill);
    int  stageRead(std::uint16_t key, std::uint32_t off,
                   std::uint8_t* out, std::uint32_t len);
    void stageDrop(std::uint16_t key);
    void stageClear();
    int  ensureStageFile();

    // ---- commit engines ----------------------------------------------------
    void commit();
    void commitBasic();
    void commitCpm();
    std::string uniqueFatName(const std::string& base, const std::string& ext);

    int  ensureOpen(const std::string& filename, bool writable, FIL*& out);
    void closeOpen();
    // Second handle for commit-time copies from a previous owner's container
    int  ensureAux(const std::string& filename, FIL*& out);
    void closeAux();

    // ---- members -----------------------------------------------------------
    Fs            fs_;
    bool          valid_ = false;
    std::string   dir_;
    std::string   path_scratch_;

    std::uint8_t* dir_image_ = nullptr;  // logical (non-inverted) directory
    std::uint8_t* prev_dir_  = nullptr;  // snapshot the last commit ran against
    std::uint8_t  dinfo_[256] = {};      // BASIC sector-map block (logical)

    // BASIC: FAT container per directory slot (empty = virtual, e.g. BRD)
    std::string   slot_fat_[64];
    // CP/M: identity (user + 11-char name, attribute bits masked) -> container
    struct CpmFile {
        std::uint8_t user;
        std::uint8_t name[11];
        std::string  fat_name;
    };
    CpmFile*      cpm_files_ = nullptr;  // kCpmMaxEnts entries
    std::uint8_t  cpm_file_count_ = 0;

    StageEnt*     stage_ = nullptr;      // kMaxStage entries
    std::uint16_t stage_count_ = 0;
    std::uint16_t stage_slots_ = 0;      // slots allocated in the temp file
    FIL           stage_file_{};
    bool          stage_open_ = false;

    std::uint8_t* scratch_ = nullptr;    // one guest sector (512 B)

    OpenFile      cur_{};
    OpenFile      aux_{};                // commit-time copy source

    // Commit-time slot-move tracking (BASIC only); heap-side, the core
    // stacks are ~2 KB and 64 strings do not fit there
    std::string*  bas_moved_ = nullptr;

    bool          dir_dirty_ = false;
    bool          in_commit_ = false;
    bool          write_error_ = false;

    // memo for the block -> file scan on the fetch path
    mutable std::int32_t  memo_block_ = -1;
    mutable int           memo_idx_ = -1;
    mutable std::uint32_t memo_off_ = 0;
};

namespace ByteSourceFactory {
    static inline int from_fdcdir(const std::string& path, FDCDirSource::Fs fs,
                                  std::uint32_t cache_size,
                                  std::unique_ptr<ByteSource>& out)
    {
        FDCDirSource* src = new (std::nothrow) FDCDirSource(path, fs, cache_size);
        if (!src) { out.reset(); return -1; } // out of RAM
        const int ret = src->valid() ? 0 : -1;
        out.reset(src);
        return ret;
    }
}
