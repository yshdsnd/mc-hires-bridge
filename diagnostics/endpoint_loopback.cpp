#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"Usage:\n"
                L"  endpoint_loopback.exe <output_pcm_file> <duration_sec> [discontinuity_log.tsv]\n");
        return 2;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;

    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mix = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)) ||
        FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) ||
        FAILED(client->GetMixFormat(&mix))) {
        return 4;
    }

    const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
    const bool floating = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22 &&
         extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const auto frameSize = mix->nBlockAlign;
    const auto sampleRate = mix->nSamplesPerSec;

    const auto initialized = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                1000000, 0, mix, nullptr);
    printf("rate=%lu channels=%u bits=%u float=%d init=%08lx\n",
           mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, floating, ULONG(initialized));

    CoTaskMemFree(mix);
    if (!floating || FAILED(initialized) || FAILED(client->GetService(IID_PPV_ARGS(&capture)))) return 5;

    FILE* file = nullptr;
    if (_wfopen_s(&file, argv[1], L"wb")) return 6;

    // Discontinuity log file (explicit via argv[3] or default <output_pcm_file>.discontinuity.tsv)
    std::wstring logPath;
    if (argc >= 4) {
        logPath = argv[3];
    } else {
        logPath = std::wstring(argv[1]) + L".discontinuity.tsv";
    }

    FILE* logFile = nullptr;
    if (!logPath.empty()) {
        _wfopen_s(&logFile, logPath.c_str(), L"w");
        if (logFile) {
            fprintf(logFile, "saved_frame\ttime_ms\tdevice_pos\tqpc_pos\tmissing_frames\tflags\n");
            fflush(logFile);
        }
    }

    if (FAILED(client->Start())) {
        if (logFile) fclose(logFile);
        fclose(file);
        return 7;
    }

    puts("READY");
    fflush(stdout);

    const auto until = GetTickCount64() + 1000ull * _wtoi(argv[2]);
    unsigned discontinuities = 0;
    uint64_t savedFrames = 0;
    uint64_t lastEndDevPos = 0;
    bool hasLastEndDevPos = false;
    unsigned packetCount = 0;

    while (GetTickCount64() < until) {
        UINT32 packets = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packets)) && packets) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT32 frames = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;

            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) {
                if (logFile) fclose(logFile);
                fclose(file);
                return 8;
            }

            ++packetCount;

            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                ++discontinuities;
                const double timeMs = (static_cast<double>(savedFrames) * 1000.0) / static_cast<double>(sampleRate);
                int64_t missingFrames = -1; // -1 represents unknown

                if (hasLastEndDevPos && devPos > lastEndDevPos) {
                    missingFrames = static_cast<int64_t>(devPos - lastEndDevPos);
                }

                if (missingFrames >= 0) {
                    printf("discontinuity: packet=%u saved_frame=%llu time_ms=%.3f device_pos=%llu qpc_pos=%llu missing_frames=%lld flags=0x%lx\n",
                           packetCount, savedFrames, timeMs, devPos, qpcPos, missingFrames, flags);
                } else {
                    printf("discontinuity: packet=%u saved_frame=%llu time_ms=%.3f device_pos=%llu qpc_pos=%llu missing_frames=unknown flags=0x%lx\n",
                           packetCount, savedFrames, timeMs, devPos, qpcPos, flags);
                }
                fflush(stdout);

                if (logFile) {
                    if (missingFrames >= 0) {
                        fprintf(logFile, "%llu\t%.3f\t%llu\t%llu\t%lld\t0x%lx\n",
                                savedFrames, timeMs, devPos, qpcPos, missingFrames, flags);
                    } else {
                        fprintf(logFile, "%llu\t%.3f\t%llu\t%llu\tunknown\t0x%lx\n",
                                savedFrames, timeMs, devPos, qpcPos, flags);
                    }
                    fflush(logFile);
                }
            }

            if (devPos != 0 || frames != 0) {
                lastEndDevPos = devPos + frames;
                hasLastEndDevPos = true;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                const std::vector<BYTE> silence(frames * frameSize, 0);
                fwrite(silence.data(), frameSize, frames, file);
            } else {
                fwrite(data, frameSize, frames, file);
            }
            savedFrames += frames;

            if (FAILED(capture->ReleaseBuffer(frames))) {
                if (logFile) fclose(logFile);
                fclose(file);
                return 9;
            }
        }
        Sleep(5);
    }

    client->Stop();
    fclose(file);
    if (logFile) fclose(logFile);

    printf("discontinuities=%u\n", discontinuities);
    return 0;
}
