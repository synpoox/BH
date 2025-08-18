#pragma once
#include <string>
#include <vector>
#include <windows.h>
#include <dsound.h>

namespace Sound {

// Call once after the game window exists.
bool Init(HWND hWnd);

// Call occasionally to release finished buffers (cheap).
void Pump();

// Play a WAV from BH.dll\sounds\<filename> (or absolute/relative path).
// Returns true if queued for playback.
bool PlayWav(const std::string& filename);

// Optional: turn on/off debug prints
void SetDebug(bool enabled);

}
