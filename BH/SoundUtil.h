#pragma once
#include <string>

namespace BH {
    // Play a wav file located in "sounds/" relative to BH.dll
    bool PlayCustomWav(const std::string& filename);
}
