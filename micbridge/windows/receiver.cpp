// MicBridge v1: 48 kHz, mono, signed little-endian 16-bit PCM in 5 ms UDP packets.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>
#include "policyformat.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { if (p) p->Release(); p = nullptr; return &p; }
    T* operator->() const { return p; }
};
static void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[256];
        snprintf(message, sizeof(message), "%s: Windows error 0x%08lx", operation, (unsigned long)result);
        throw std::runtime_error(message);
    }
}
static uint32_t be32(const unsigned char* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static unsigned be16(const unsigned char* p) { return unsigned(p[0]) << 8 | p[1]; }
static void put32(unsigned char* p, uint32_t value) {
    for (int i = 3; i >= 0; --i) { p[i] = (unsigned char)value; value >>= 8; }
}
static void put64(unsigned char* p, uint64_t value) {
    for (int i = 7; i >= 0; --i) { p[i] = (unsigned char)value; value >>= 8; }
}
static volatile LONG running = 1;
static BOOL WINAPI control(DWORD) { InterlockedExchange(&running, 0); return TRUE; }

static void normalizeSteamPair(IMMDeviceEnumerator* enumerator) {
    Com<IPolicyFormat> policy;
    check(CoCreateInstance(MicPolicyClass, nullptr, CLSCTX_ALL,
                           MicPolicyInterface, reinterpret_cast<void**>(policy.put())), "Open Steam mic format policy");
    for (auto flow : {eRender, eCapture}) {
        Com<IMMDeviceCollection> endpoints;
        check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, endpoints.put()), "Find Steam microphone pair");
        UINT count = 0; check(endpoints->GetCount(&count), "Count Steam endpoints");
        bool matched = false;
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> device; check(endpoints->Item(i, device.put()), "Get Steam endpoint");
            Com<IPropertyStore> properties; check(device->OpenPropertyStore(STGM_READ, properties.put()), "Get Steam endpoint properties");
            PROPVARIANT name; PropVariantInit(&name);
            check(properties->GetValue(PKEY_Device_FriendlyName, &name), "Get Steam endpoint name");
            const std::wstring text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
            PropVariantClear(&name);
            if (text.find(L"Steam Streaming Microphone") == std::wstring::npos) continue;
            matched = true;
            LPWSTR id = nullptr; check(device->GetId(&id), "Get Steam endpoint ID");
            WAVEFORMATEX* previous = nullptr;
            HRESULT result = policy->GetDeviceFormat(id, FALSE, &previous);
            if (FAILED(result)) { CoTaskMemFree(id); check(result, "Query Steam endpoint device format"); }
            const bool correct = previous && previous->nChannels == 2 && previous->nSamplesPerSec == 48000 && previous->wBitsPerSample == 32;
            if (previous) {
                printf("Steam %s device format: %lu Hz, %u channels, %u-bit%s\n",
                       flow == eRender ? "render" : "capture", previous->nSamplesPerSec,
                       previous->nChannels, previous->wBitsPerSample, correct ? " (matched)" : " (normalizing)");
            }
            if (!correct) {
                WAVEFORMATEXTENSIBLE format{};
                format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                format.Format.nChannels = 2; format.Format.nSamplesPerSec = 48000;
                format.Format.wBitsPerSample = 32; format.Format.nBlockAlign = 8;
                format.Format.nAvgBytesPerSec = 384000; format.Format.cbSize = 22;
                format.Samples.wValidBitsPerSample = 32;
                format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                format.SubFormat = MicPcmSubtype;
                WAVEFORMATEXTENSIBLE previousFormat{};
                result = policy->SetDeviceFormat(id, &format.Format, &previousFormat.Format);
            }
            CoTaskMemFree(previous); CoTaskMemFree(id);
            check(result, "Set Steam microphone pair to stereo 32-bit 48 kHz");
        }
        if (!matched) throw std::runtime_error("Both Steam microphone render and capture endpoints must be enabled");
    }
    fflush(stdout);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring deviceName = L"Steam Streaming Microphone", allowed;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--list") list = true;
        else if (arg == L"--device" && i + 1 < argc) deviceName = argv[++i];
        else if (arg == L"--allow" && i + 1 < argc) allowed = argv[++i];
        else { fputs("Usage: micbridge-receiver.exe --allow CLIENT_IPV4 [--device NAME]\n       micbridge-receiver.exe --list\n", stderr); return 2; }
    }
    SOCKET socket = INVALID_SOCKET;
    HANDLE event = nullptr, instance = nullptr, priority = nullptr;
    WSAEVENT networkEvent = WSA_INVALID_EVENT;
    bool comInitialized = false, winsockInitialized = false;
    int exitCode = 0;
    try {
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
        comInitialized = true;
        Com<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put())), "Enumerate audio devices");
        Com<IMMDeviceCollection> devices;
        check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put()), "List playback endpoints");
        UINT count = 0; check(devices->GetCount(&count), "Count endpoints");
        Com<IMMDevice> selected;
        std::wstring selectedName;
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> device;
            check(devices->Item(i, device.put()), "Read endpoint");
            Com<IPropertyStore> properties;
            check(device->OpenPropertyStore(STGM_READ, properties.put()), "Read endpoint properties");
            PROPVARIANT name; PropVariantInit(&name);
            check(properties->GetValue(PKEY_Device_FriendlyName, &name), "Read endpoint name");
            std::wstring text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
            PropVariantClear(&name);
            if (list) wprintf(L"%ls\n", text.c_str());
            if (!selected.p && !deviceName.empty() && text.find(deviceName) != std::wstring::npos) {
                selected.p = device.p; selected.p->AddRef(); selectedName = text;
            }
        }
        if (!list) {
            if (!selected.p) throw std::runtime_error("Virtual microphone playback endpoint not found. Run --list; install/enable Steam Streaming Microphone or VB-CABLE. No physical speaker fallback is used.");
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Initialize Winsock failed");
            winsockInitialized = true;
            IN_ADDR client{};
            if (InetPtonW(AF_INET, allowed.c_str(), &client) != 1)
                throw std::runtime_error("Specify the Linux client's IPv4 address with --allow");
            instance = CreateMutexW(nullptr, FALSE, L"Local\\MicBridgeReceiver-39842");
            if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("MicBridge receiver already running in this session");
            const bool steam = selectedName.find(L"Steam Streaming Microphone") != std::wstring::npos;
            if (steam) normalizeSteamPair(enumerator.p);
            socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket == INVALID_SOCKET) throw std::runtime_error("Create UDP socket failed");
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(39842);
            if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) throw std::runtime_error("Bind UDP 39842 failed (another receiver may be running)");
            u_long nonblocking = 1; ioctlsocket(socket, FIONBIO, &nonblocking);
            networkEvent = WSACreateEvent();
            if (networkEvent == WSA_INVALID_EVENT || WSAEventSelect(socket, networkEvent, FD_READ) == SOCKET_ERROR)
                throw std::runtime_error("Create microphone network event failed");
            int receiveBuffer = 65536;
            setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
            Com<IAudioClient> audio;
            check(selected->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(audio.put())), "Open virtual microphone playback");
            WAVEFORMATEXTENSIBLE format{};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = 2; format.Format.nSamplesPerSec = 48000;
            format.Format.wBitsPerSample = 32; format.Format.nBlockAlign = 8;
            format.Format.nAvgBytesPerSec = 384000; format.Format.cbSize = 22;
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            format.SubFormat = MicFloatSubtype;
            check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (steam ? 0 : AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY),
                                    0, 0, &format.Format, nullptr), "Initialize stereo float32 48 kHz microphone render stream");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) throw std::runtime_error("Create audio event failed");
            check(audio->SetEventHandle(event), "Set audio event");
            UINT32 bufferFrames = 0; check(audio->GetBufferSize(&bufferFrames), "Get audio buffer size");
            REFERENCE_TIME defaultPeriod = 0;
            check(audio->GetDevicePeriod(&defaultPeriod, nullptr), "Get microphone output period");
            // Keep one engine period queued, rather than filling the entire device buffer.
            const UINT32 targetFrames = std::min(bufferFrames, std::max(UINT32(240), UINT32((defaultPeriod * 48000 + 9999999) / 10000000)));
            Com<IAudioRenderClient> render;
            check(audio->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.put())), "Get render service");
            BYTE* output = nullptr;
            check(render->GetBuffer(targetFrames, &output), "Prime audio output");
            check(render->ReleaseBuffer(targetFrames, AUDCLNT_BUFFERFLAGS_SILENT), "Prime silence");
            DWORD taskIndex = 0; priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            SetConsoleCtrlHandler(control, TRUE);
            check(audio->Start(), "Start microphone output");
            wprintf(L"MicBridge 1.2: UDP 39842 from %ls -> %ls\nSelect Microphone (Steam Streaming Microphone) in your voice app.\n", allowed.c_str(), selectedName.c_str());
            printf("Network: 48 kHz mono PCM, 5 ms packets; output: stereo float32 48 kHz.\n15 ms startup jitter buffer; WASAPI capacity %.1f ms, target padding %.1f ms.\n", bufferFrames / 48.0, targetFrames / 48.0);
            fflush(stdout);
            std::map<uint32_t, std::array<int16_t, 240>> queue;
            std::array<int16_t, 240> current{};
            unsigned offset = 240;
            uint32_t session = 0, next = 0;
            bool active = false, primed = false;
            ULONGLONG lastPacket = 0, lastLog = GetTickCount64();
            ULONGLONG lastFeedback = 0;
            sockaddr_in feedbackPeer{};
            uint32_t echoSequence = 0;
            uint64_t received = 0, missing = 0, late = 0, underruns = 0, trimmed = 0;
            while (InterlockedCompareExchange(&running, 1, 1)) {
                HANDLE events[] = {event, networkEvent};
                const DWORD ready = WaitForMultipleObjects(2, events, FALSE, 20);
                if (ready == WAIT_FAILED) throw std::runtime_error("Wait for microphone input/output failed");
                WSANETWORKEVENTS network{};
                if (WSAEnumNetworkEvents(socket, networkEvent, &network) == SOCKET_ERROR)
                    throw std::runtime_error("Read microphone network events failed");
                // Drain a bounded batch so an incoming flood cannot starve WASAPI.
                for (int batch = 0; batch < 128; ++batch) {
                    unsigned char packet[1024]; sockaddr_in peer{}; int peerSize = sizeof(peer);
                    int bytes = recvfrom(socket, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                                         reinterpret_cast<sockaddr*>(&peer), &peerSize);
                    if (bytes == SOCKET_ERROR) {
                        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                        if (WSAGetLastError() == WSAEMSGSIZE || WSAGetLastError() == WSAECONNRESET) continue;
                        throw std::runtime_error("Receive microphone UDP failed");
                    }
                    if (peer.sin_addr.s_addr != client.s_addr || bytes < 16 || memcmp(packet, "MCB1", 4)) continue;
                    uint32_t id = be32(packet + 4), sequence = be32(packet + 8);
                    const unsigned flags = be16(packet + 12), frames = be16(packet + 14);
                    if (flags == 1 && frames == 0 && bytes == 16) {
                        if (active && session == id) { active = primed = false; queue.clear(); offset = 240; }
                        continue;
                    }
                    if (flags || frames != 240 || bytes != 496) continue;
                    if (!active || id != session || GetTickCount64() - lastPacket > 300) {
                        queue.clear(); offset = 240; next = sequence; session = id; active = true; primed = false;
                        received = missing = late = underruns = trimmed = 0;
                        lastFeedback = 0;
                        puts("Microphone stream connected."); fflush(stdout);
                    }
                    lastPacket = GetTickCount64(); ++received;
                    feedbackPeer = peer; echoSequence = sequence;
                    const int32_t distance = int32_t(sequence - next);
                    if (distance < 0) { ++late; continue; }
                    if (distance > 64) { queue.clear(); offset = 240; next = sequence; primed = false; ++trimmed; }
                    auto& samples = queue[sequence];
                    for (unsigned i = 0; i < 240; ++i)
                        samples[i] = int16_t(uint16_t(packet[16 + 2 * i]) | uint16_t(packet[17 + 2 * i]) << 8);
                    if (queue.size() > 6) {
                        // Keep enough headroom for batched Linux capture delivery,
                        // while trimming old audio back to 20 ms when it grows.
                        offset = 240;
                        while (queue.size() > 4) {
                            queue.erase(next++); ++trimmed;
                        }
                    }
                }
                const auto now = GetTickCount64();
                if (active && now - lastPacket > 300) {
                    active = primed = false; queue.clear(); offset = 240;
                    puts("Microphone stream idle; output is silent."); fflush(stdout);
                }
                if (!primed && queue.size() >= 3) primed = true;
                if (ready == WAIT_OBJECT_0 || WaitForSingleObject(event, 0) == WAIT_OBJECT_0) {
                    UINT32 padding = 0; check(audio->GetCurrentPadding(&padding), "Get microphone padding");
                    const UINT32 framesAvailable = padding < targetFrames ? targetFrames - padding : 0;
                    if (framesAvailable) {
                        check(render->GetBuffer(framesAvailable, &output), "Get microphone output buffer");
                        auto* pcm = reinterpret_cast<float*>(output);
                        bool starved = false;
                        for (UINT32 i = 0; i < framesAvailable; ++i) {
                            if (!primed) { pcm[2 * i] = pcm[2 * i + 1] = 0; continue; }
                            if (offset == 240) {
                                auto found = queue.find(next);
                                if (found != queue.end()) { current = found->second; queue.erase(found); ++next; offset = 0; }
                                else if (!queue.empty()) { current.fill(0); ++next; offset = 0; ++missing; }
                                else { pcm[2 * i] = pcm[2 * i + 1] = 0; starved = true; continue; }
                            }
                            pcm[2 * i] = pcm[2 * i + 1] = current[offset++] / 32768.0f;
                        }
                        if (starved && active) ++underruns;
                        check(render->ReleaseBuffer(framesAvailable, 0), "Submit microphone output");
                    }
                }
                if (now - lastLog >= 5000 && active) {
                    printf("packets=%llu missing=%llu late=%llu underruns=%llu trims=%llu queued=%zu\n",
                           (unsigned long long)received, (unsigned long long)missing, (unsigned long long)late,
                           (unsigned long long)underruns, (unsigned long long)trimmed, queue.size());
                    fflush(stdout); lastLog = now;
                }
                if (active && now - lastFeedback >= 500) {
                    UINT32 padding = 0;
                    check(audio->GetCurrentPadding(&padding), "Get feedback padding");
                    unsigned char report[72]{};
                    memcpy(report, "MCS1", 4);
                    put32(report + 4, session); put32(report + 8, echoSequence);
                    put32(report + 12, uint32_t(queue.size() * 240 + (offset < 240 ? 240 - offset : 0)));
                    put32(report + 16, padding); put32(report + 20, bufferFrames);
                    put64(report + 24, received); put64(report + 32, missing); put64(report + 40, late);
                    put64(report + 48, underruns); put64(report + 56, trimmed);
                    put32(report + 68, primed ? 1 : 0);
                    sendto(socket, reinterpret_cast<const char*>(report), sizeof(report), 0,
                           reinterpret_cast<const sockaddr*>(&feedbackPeer), sizeof(feedbackPeer));
                    lastFeedback = now;
                }
            }
            audio->Stop();
        }
    } catch (const std::exception& e) { fprintf(stderr, "MicBridge: %s\n", e.what()); exitCode = 1; }
    if (priority) AvRevertMmThreadCharacteristics(priority);
    if (socket != INVALID_SOCKET) closesocket(socket);
    if (networkEvent != WSA_INVALID_EVENT) WSACloseEvent(networkEvent);
    if (event) CloseHandle(event);
    if (instance) CloseHandle(instance);
    if (winsockInitialized) WSACleanup();
    if (comInitialized) CoUninitialize();
    return exitCode;
}
