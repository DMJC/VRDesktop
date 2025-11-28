#ifndef CONFIG_H
#define CONFIG_H

#include <iostream>
#include <fstream>
#include <string>

struct Config {
    std::string displayOutput;
    bool curved = false;
    float distance = 0.0f;
    bool hide_window = false;
};

bool loadConfig(const std::string &filename, Config &cfg);
bool saveConfig(const std::string &filename, const Config &cfg);

#endif //CONFIG_H
