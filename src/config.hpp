#pragma once
#include <vector>
#include <string>


#define MAX_CONFIG_KEY_LENGTH 16
#define MAX_CONFIG_VALUE_LENGTH 64



using SectionConfig = std::vector<std::pair<std::string, std::string>>;
extern std::vector<std::pair<std::string, SectionConfig>> picoConfig;
