#include "SoundUtil.h"
#include <mmsystem.h>
#include <mmreg.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace {
    LPDIRECTSOUND8                 g_pDS  = nullptr;
    HWND                           g_hWnd = nullptr;
    bool                           g_debug = false;

    struct ActiveBuf {
        LPDIRECTSOUNDBUFFER buf = nullptr;
    };
    std::vector<ActiveBuf> g_active;

    void debugPrint(const char* fmt, ...) {
        if (!g_debug) return;
        char buf[512];
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
        va_end(ap);
        OutputDebugStringA(buf);
    }

    // Return BH.dll folder
    std::string GetBHFolder() {
        char modulePath[MAX_PATH] = {0};
        HMODULE h = GetModuleHandleA("BH.dll");
        if (!h) return std::string();
        if (!GetModuleFileNameA(h, modulePath, MAX_PATH)) return std::string();
        PathRemoveFileSpecA(modulePath);
        return std::string(modulePath);
    }

    // Quick “is absolute?” check
    bool IsAbsolutePath(const std::string& p) {
        if (p.size() >= 2 && p[1] == ':') return true;          // C:\...
        if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') return true; // \\server\...
        if (!p.empty() && (p[0] == '\\' || p[0] == '/')) return true;
        return false;
    }

    // Builds full path. If not absolute, tries BH.dll\sounds\<filename>
    std::string ResolvePath(const std::string& filename) {
        if (IsAbsolutePath(filename)) return filename;
        std::string base = GetBHFolder();
        if (base.empty()) return filename;
        std::string full = base + "\\sounds\\" + filename;
        return full;
    }

    // Load a PCM WAV from disk into wfx + raw bytes (returns false on failure)
    bool LoadWavFile(const std::string& path, WAVEFORMATEX& outFmt, std::vector<BYTE>& outData) {
        HMMIO hMmio = mmioOpenA((LPSTR)path.c_str(), NULL, MMIO_READ | MMIO_ALLOCBUF);
        if (!hMmio) { debugPrint("[BH] mmioOpen failed: %s\n", path.c_str()); return false; }

        MMCKINFO riffChunk = {};
        riffChunk.fccType = mmioFOURCC('W','A','V','E');
        if (mmioDescend(hMmio, &riffChunk, NULL, MMIO_FINDRIFF)) { mmioClose(hMmio, 0); return false; }

        MMCKINFO fmtChunk = {};
        fmtChunk.ckid = mmioFOURCC('f','m','t',' ');
        if (mmioDescend(hMmio, &fmtChunk, &riffChunk, MMIO_FINDCHUNK)) { mmioClose(hMmio, 0); return false; }

        // Read WAVEFORMATEX or WAVEFORMATEXTENSIBLE header (we’ll accept PCM/IEEE_FLOAT but will only
        // create buffers for PCM; if you need float, convert or reject).
        std::vector<BYTE> fmtBytes(fmtChunk.cksize);
        if (mmioRead(hMmio, (HPSTR)fmtBytes.data(), (LONG)fmtBytes.size()) != (LONG)fmtBytes.size()) {
            mmioClose(hMmio, 0); return false;
        }
        mmioAscend(hMmio, &fmtChunk, 0);

        // Basic parse
        if (fmtBytes.size() < sizeof(WAVEFORMATEX)) { mmioClose(hMmio, 0); return false; }
        const WAVEFORMATEX* wfx = reinterpret_cast<const WAVEFORMATEX*>(fmtBytes.data());

        // Only allow PCM here (WAVE_FORMAT_PCM or WAVE_FORMAT_EXTENSIBLE with PCM subformat)
        WAVEFORMATEX finalFmt = {};
        finalFmt.wFormatTag = wfx->wFormatTag;
        finalFmt.nChannels  = wfx->nChannels;
        finalFmt.nSamplesPerSec = wfx->nSamplesPerSec;
        finalFmt.wBitsPerSample = wfx->wBitsPerSample;
        finalFmt.nBlockAlign    = wfx->nBlockAlign;
        finalFmt.nAvgBytesPerSec= wfx->nAvgBytesPerSec;
        finalFmt.cbSize         = 0;

        if (!(finalFmt.wFormatTag == WAVE_FORMAT_PCM)) {
            // Very small enhancement: accept Extensible PCM
            if (finalFmt.wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
                debugPrint("[BH] Unsupported WAV format tag: %u\n", finalFmt.wFormatTag);
                mmioClose(hMmio, 0); return false;
            }
            // You could parse the extensible header and require PCM subformat; for brevity we’ll bail.
            debugPrint("[BH] WAVE_FORMAT_EXTENSIBLE not supported in this snippet\n");
            mmioClose(hMmio, 0); return false;
        }

        MMCKINFO dataChunk = {};
        dataChunk.ckid = mmioFOURCC('d','a','t','a');
        if (mmioDescend(hMmio, &dataChunk, &riffChunk, MMIO_FINDCHUNK)) { mmioClose(hMmio, 0); return false; }

        outData.resize(dataChunk.cksize);
        if (mmioRead(hMmio, (HPSTR)outData.data(), dataChunk.cksize) != (LONG)dataChunk.cksize) {
            mmioClose(hMmio, 0); return false;
        }
        mmioClose(hMmio, 0);

        outFmt = finalFmt;
        return true;
    }

    // Create a DS secondary buffer from PCM bytes
    LPDIRECTSOUNDBUFFER CreateSecondaryBuffer(const WAVEFORMATEX& wfx, const std::vector<BYTE>& data) {
        if (!g_pDS) return nullptr;

        DSBUFFERDESC desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_CTRLVOLUME; // you can add DSBCAPS_GLOBALFOCUS if you want audio when not foreground
        desc.dwBufferBytes = (DWORD)data.size();
        desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&wfx);

        LPDIRECTSOUNDBUFFER pBuf = nullptr;
        HRESULT hr = g_pDS->CreateSoundBuffer(&desc, &pBuf, NULL);
        if (FAILED(hr) || !pBuf) {
            debugPrint("[BH] CreateSoundBuffer failed: 0x%08X\n", hr);
            return nullptr;
        }

        // Copy PCM into buffer
        VOID* pLock1 = nullptr; DWORD dwSize1 = 0;
        VOID* pLock2 = nullptr; DWORD dwSize2 = 0;
        hr = pBuf->Lock(0, desc.dwBufferBytes, &pLock1, &dwSize1, &pLock2, &dwSize2, 0);
        if (FAILED(hr)) {
            pBuf->Release();
            debugPrint("[BH] Lock failed: 0x%08X\n", hr);
            return nullptr;
        }

        memcpy(pLock1, data.data(), dwSize1);
        if (pLock2 && dwSize2) {
            memcpy(pLock2, data.data() + dwSize1, dwSize2);
        }
        pBuf->Unlock(pLock1, dwSize1, pLock2, dwSize2);

        return pBuf;
    }

} // anon

namespace Sound {

bool Init(HWND hWnd) {
    g_hWnd = hWnd;
    if (g_pDS) return true; // already inited

    if (FAILED(DirectSoundCreate8(NULL, &g_pDS, NULL))) {
        debugPrint("[BH] DirectSoundCreate8 failed\n");
        return false;
    }
    if (FAILED(g_pDS->SetCooperativeLevel(hWnd, DSSCL_PRIORITY))) {
        debugPrint("[BH] SetCooperativeLevel failed\n");
        g_pDS->Release(); g_pDS = nullptr;
        return false;
    }
    return true;
}

void Pump() {
    // Release finished buffers
    for (size_t i = 0; i < g_active.size(); ) {
        DWORD status = 0;
        if (!g_active[i].buf || FAILED(g_active[i].buf->GetStatus(&status))) {
            if (g_active[i].buf) g_active[i].buf->Release();
            g_active.erase(g_active.begin() + i);
            continue;
        }
        if ((status & DSBSTATUS_PLAYING) == 0) {
            g_active[i].buf->Release();
            g_active.erase(g_active.begin() + i);
            continue;
        }
        ++i;
    }
}

bool PlayWav(const std::string& filename) {
    if (!g_pDS) return false;

    const std::string fullPath = ResolvePath(filename);
    WAVEFORMATEX wfx = {};
    std::vector<BYTE> pcm;
    if (!LoadWavFile(fullPath, wfx, pcm)) {
        debugPrint("[BH] LoadWavFile failed: %s\n", fullPath.c_str());
        return false;
    }

    // Optionally cap concurrent voices (e.g. 16)
    const size_t kMaxVoices = 16;
    if (g_active.size() >= kMaxVoices) {
        // Drop oldest finished or, if all playing, drop the request.
        Pump();
        if (g_active.size() >= kMaxVoices) return false;
    }

    LPDIRECTSOUNDBUFFER pBuf = CreateSecondaryBuffer(wfx, pcm);
    if (!pBuf) return false;

    // Start playback, no loop
    if (FAILED(pBuf->Play(0, 0, 0))) {
        pBuf->Release();
        return false;
    }

    g_active.push_back({ pBuf });
    return true;
}

void SetDebug(bool enabled) { g_debug = enabled; }

} // namespace Sound
