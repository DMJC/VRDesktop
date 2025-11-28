#include "config.h"

bool loadConfig(const std::string &filename, Config &cfg)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << filename << std::endl;
        return false;
    }

    std::string line;

    // Line 1: DisplayOutput
    if (!std::getline(file, cfg.displayOutput)) return false;

    // Line 2: curved / flat
    if (!std::getline(file, line)) return false;
    if (line == "curved")
        cfg.curved = true;
    else if (line == "flat")
        cfg.curved = false;
    else {
        std::cerr << "Invalid curvature value: " << line << std::endl;
        return false;
    }

    // Line 3: distance (float)
    if (!std::getline(file, line)) return false;
    try {
        cfg.distance = std::stof(line);
    } catch (...) {
        std::cerr << "Invalid float value for distance: " << line << std::endl;
        return false;
    }

    // Line 4: enabled / disabled
    if (!std::getline(file, line)) return false;
    if (line == "enabled")
        cfg.hide_window = false;
    else if (line == "disabled")
        cfg.hide_window = true;
    else {
        std::cerr << "Invalid show_window value: " << line << std::endl;
        return false;
    }

    return true;
}

bool saveConfig(const std::string &filename, const Config &cfg)
{
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file for writing: " << filename << std::endl;
        return false;
    }
    file << cfg.displayOutput << "\n";
    file << (cfg.curved ? "curved" : "flat") << "\n";
    file << cfg.distance << "\n";
    file << (cfg.hide_window ? "disabled" : "enabled") << "\n";
    return true;
}
