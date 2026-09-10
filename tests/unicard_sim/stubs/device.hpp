#pragma once
#include <string>
#include <vector>
#include <cstdint>
struct FDCDevice {
    std::vector<std::string> mounted = std::vector<std::string>(4);
    int setDriveContent(uint8_t d, const char* p) { if (d > 3) return -1; mounted[d] = p; return 1; }
    void ejectDrive(uint8_t d) { if (d <= 3) mounted[d].clear(); }
    const std::string& currentImage(uint8_t d) const { return mounted[d]; }
};
struct QDDevice { std::string path; void setDriveContent(const std::string& p) { path = p; } const std::string& currentImage() const { return path; } bool hasDisk() const { return !path.empty(); } };
extern FDCDevice* fdc;
extern QDDevice* qd;
