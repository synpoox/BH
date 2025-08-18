#include "SoundUtil.h"
#include <windows.h>
#include <string>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace BH {

    bool PlayCustomWav(const std::string& filename) {
        // Get BH.dll folder
        char modulePath[MAX_PATH];
        if (!GetModuleFileNameA(GetModuleHandleA("BH.dll"), modulePath, MAX_PATH)) {
            return false;
        }

        std::string basePath(modulePath);
        size_t pos = basePath.find_last_of("\\/");
        if (pos != std::string::npos) {
            basePath = basePath.substr(0, pos);
        }

        // Append "sounds/<filename>"
        std::string fullPath = basePath + "\\sounds\\" + filename;

        // Play sound asynchronously
        BOOL ok = PlaySoundA(fullPath.c_str(), NULL,
                             SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return ok == TRUE;
    }

}
