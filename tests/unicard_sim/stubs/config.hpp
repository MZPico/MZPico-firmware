#pragma once
#include <string>
#include <vector>
#include <utility>
using SectionConfig = std::vector<std::pair<std::string, std::string>>;
extern std::vector<std::pair<std::string, SectionConfig>> picoConfig;
