#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include "ff.h"
#include "mz_devices.hpp"
#include "byte_source.hpp"

#define QD_PORTS      4
#define QDISK_FORMAT_SIZE            82958
#define QDISK_IMAGE_MAX_SIZE         (QDISK_FORMAT_SIZE + 84 + 0x1a80)

#define QDISK_DISCONNECTED           0
#define QDISK_CONNECTED              1

#define QDSIO_CHANNEL_A              0
#define QDSIO_CHANNEL_B              1

#define QDSTS_NO_DISC                0
#define QDSTS_IMG_READY              1
#define QDSTS_HEAD_HOME              2
#define QDSTS_IMG_SYNC               4
#define QDSTS_IMG_READONLY           8

constexpr bool QD_EXWAIT = true;
constexpr const char QD_ID[] = "qd";
constexpr uint8_t QD_DEFAULT_BASE_PORT = 0xf4;

typedef enum en_QDSIO_ADDR {
    QDSIO_ADDR_DATA_A = 0,
    QDSIO_ADDR_DATA_B,
    QDSIO_ADDR_CTRL_A,
    QDSIO_ADDR_CTRL_B
} en_QDSIO_ADDR;

typedef enum en_QDSIO_REGADRR {
    QDSIO_REGADDR_0 = 0,
    QDSIO_REGADDR_1,
    QDSIO_REGADDR_2,
    QDSIO_REGADDR_3,
    QDSIO_REGADDR_4,
    QDSIO_REGADDR_5,
    QDSIO_REGADDR_6,
    QDSIO_REGADDR_7
} en_QDSIO_REGADRR;

typedef enum en_QDSIO_WR0CMD {
    QDSIO_WR0CMD_NONE = 0,
    QDSIO_WR0CMD_SDLC_STOP,
    QDSIO_WR0CMD_RESET_INTF,
    QDSIO_WR0CMD_RESET,
    QDSIO_WR0CMD_ENABLE_INT,
    QDSIO_WR0CMD_RESET_OUTBUF_INT,
    QDSIO_WR0CMD_RESET_ERRFL,
    QDSIO_WR0CMD_RETI
} en_QDSIO_WR0CMD;

typedef struct st_QDSIO_CHANNEL {
    char name;
    en_QDSIO_REGADRR REG_addr;
    uint8_t Wreg[8];
    uint8_t Rreg[3];
} st_QDSIO_CHANNEL;

class QDDevice final : public MZDevice {
public:
    explicit QDDevice();
    ~QDDevice();

    int init() override;
    int isInterrupt() override;
    bool needsExwait() const override { return QD_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return QD_DEFAULT_BASE_PORT; }
    int readConfig(dictionary *ini) override;
    static std::string getDevType() { return QD_ID; }

    void open();
    void close();

    void setConnected(bool on);
    void setWriteProtected(bool on);
    bool isWriteProtected() const;

    void setDriveContent(const std::string& path);
    const std::string& getStdImagePath() const { return stdPath; }
    static int readByte(MZDevice* self_, uint8_t port, uint8_t *dt, uint8_t /*high_addr*/);
    static int writeByte(MZDevice* self_, uint8_t port, uint8_t dt, uint8_t /*high_addr*/);

private:
    unsigned connected;
    unsigned status;
    st_QDSIO_CHANNEL channel[2];
    FIL image_file;
    uint16_t out_crc16;
    unsigned image_position;
    std::string stdPath;
    bool writeProtected;
    std::unique_ptr<ByteSource> bs;

    void driveReset();
    uint8_t readByteFromDrive();
    void writeByteIntoDrive(uint8_t value);
    int testDiskIsWriteable();
};
