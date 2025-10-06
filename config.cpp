#include "config.hpp"
#include "device.hpp"
#include "pico_mgr.hpp"

typedef struct {
  char key[MAX_CONFIG_KEY_LENGTH];
  char value[MAX_CONFIG_VALUE_LENGTH];
} ConfigEntry;
#define CONFIG_ENTRY_SIZE (MAX_CONFIG_KEY_LENGTH + MAX_CONFIG_VALUE_LENGTH)

void pack_ConfigEntry(const void* recPtr, uint8_t* dst) {
    const ConfigEntry* r = (const ConfigEntry*)recPtr;
    memcpy(dst, r->key, MAX_CONFIG_KEY_LENGTH);
    memcpy(dst + MAX_CONFIG_KEY_LENGTH, r->value, MAX_CONFIG_VALUE_LENGTH);
}
void unpack_ConfigEntry(const uint8_t* src, void* outPtr) {
    ConfigEntry* r = (ConfigEntry*)outPtr;
    memcpy(r->key, src, MAX_CONFIG_KEY_LENGTH);
    memcpy(r->value, src + MAX_CONFIG_KEY_LENGTH, MAX_CONFIG_VALUE_LENGTH);
}

std::unordered_map<std::string, SectionConfig> picoConfig;

int getConfig(std::string &section, PicoMgr *mgr) {
    mgr->setContent(CONFIG_ENTRY_SIZE, pack_ConfigEntry, unpack_ConfigEntry);
    auto it = picoConfig.find(section);
    if (it != picoConfig.end()) {
        for (const auto &configEntry : it->second) {
            ConfigEntry entryStruct;
            strncpy(entryStruct.key, configEntry.first.c_str(), MAX_CONFIG_KEY_LENGTH);
            strncpy(entryStruct.value, configEntry.second.c_str(), MAX_CONFIG_VALUE_LENGTH);
            mgr->addRecord(&entryStruct);
        }
    }
    return 0;
}
