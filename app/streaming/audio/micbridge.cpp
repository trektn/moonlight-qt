#include "micbridge.h"
#include <SDL.h>
#include <QRandomGenerator>
#include <QtEndian>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif

struct MicBridge::Impl {
    SDL_AudioDeviceID device = 0;
    bool audioInitialized = false;
    int socket = -1;
    quint32 session = QRandomGenerator::global()->generate(), sequence = 0;
    unsigned used = 0;
    unsigned char packet[496]{};
    std::atomic<quint64> packets{0}, errors{0};
    std::atomic<int> peak{0};
    std::atomic<bool> live{false};
    Uint64 started = 0;

    void send(bool end = false) {
#ifdef Q_OS_LINUX
        memcpy(packet, "MCB1", 4);
        qToBigEndian(session, packet + 4);
        qToBigEndian(sequence++, packet + 8);
        qToBigEndian<quint16>(end ? 1 : 0, packet + 12);
        qToBigEndian<quint16>(end ? 0 : 240, packet + 14);
        const int bytes = end ? 16 : sizeof(packet);
        if (::send(socket, packet, bytes, MSG_DONTWAIT | MSG_NOSIGNAL) == bytes) ++packets;
        else ++errors;
#else
        Q_UNUSED(end);
#endif
    }
    static void capture(void* context, Uint8* data, int length) {
        auto& self = *static_cast<Impl*>(context);
        int peak = 0;
        for (int i = 0; i + 1 < length; i += 2)
            peak = std::max(peak, std::abs(int(qFromLittleEndian<qint16>(data + i))));
        self.peak.store(peak, std::memory_order_relaxed);
        while (length > 0) {
            const unsigned bytes = std::min(unsigned(length), 480u - self.used);
            memcpy(self.packet + 16 + self.used, data, bytes);
            self.used += bytes; data += bytes; length -= int(bytes);
            if (self.used == 480) { self.send(); self.used = 0; }
        }
    }
};

MicBridge::MicBridge() : m_Impl(new Impl) {}
MicBridge::~MicBridge() {
    if (m_Impl->device) SDL_CloseAudioDevice(m_Impl->device);
#ifdef Q_OS_LINUX
    if (m_Impl->socket >= 0) {
        if (m_Impl->device) for (int i = 0; i < 3; ++i) m_Impl->send(true);
        close(m_Impl->socket);
    }
#endif
    if (m_Impl->audioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

QVariantList MicBridge::devices() {
    QVariantList result;
#ifdef Q_OS_LINUX
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return result;
    for (int i = 0; i < SDL_GetNumAudioDevices(1); ++i) {
        const char* name = SDL_GetAudioDeviceName(i, 1);
        if (name) result << QVariantMap{{"name", QString::fromUtf8(name)}};
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
    return result;
}

bool MicBridge::start(const QString& device, const QString& host, QString& error) {
#ifdef Q_OS_LINUX
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    int status = getaddrinfo(host.toUtf8().constData(), "39842", &hints, &addresses);
    if (status) { error = "MicBridge receiver address: " + QString::fromLocal8Bit(gai_strerror(status)); return false; }
    for (auto addr = addresses; addr; addr = addr->ai_next) {
        m_Impl->socket = ::socket(addr->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (m_Impl->socket >= 0 && ::connect(m_Impl->socket, addr->ai_addr, addr->ai_addrlen) == 0) break;
        if (m_Impl->socket >= 0) close(m_Impl->socket);
        m_Impl->socket = -1;
    }
    freeaddrinfo(addresses);
    if (m_Impl->socket < 0) { error = "MicBridge cannot open UDP socket."; return false; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { error = QString::fromUtf8(SDL_GetError()); return false; }
    m_Impl->audioInitialized = true;
    SDL_AudioSpec wanted{}, obtained{};
    wanted.freq = 48000; wanted.channels = 1; wanted.format = AUDIO_S16LSB;
    wanted.samples = 240; wanted.callback = Impl::capture; wanted.userdata = m_Impl.get();
    const QByteArray name = device.toUtf8();
    m_Impl->device = SDL_OpenAudioDevice(device.isEmpty() ? nullptr : name.constData(), 1, &wanted, &obtained, 0);
    if (!m_Impl->device) { error = "MicBridge microphone: " + QString::fromUtf8(SDL_GetError()); return false; }
    m_Impl->started = SDL_GetTicks64();
    m_Impl->live.store(true, std::memory_order_release);
    SDL_PauseAudioDevice(m_Impl->device, 0);
    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "MicBridge capturing %s -> %s:39842 (48 kHz mono PCM, 5 ms packets)",
                device.isEmpty() ? "default microphone" : name.constData(), qPrintable(host));
    return true;
#else
    Q_UNUSED(device); Q_UNUSED(host);
    error = "MicBridge capture is available on Linux only.";
    return false;
#endif
}

bool MicBridge::ownsDevice(unsigned id) const { return m_Impl->device == id; }

QString MicBridge::statsText() const {
    const auto& s = *m_Impl;
    if (!s.live.load(std::memory_order_acquire)) return {};
    const double seconds = std::max(0.001, double(SDL_GetTicks64() - s.started) / 1000.0);
    const int peak = s.peak.load(std::memory_order_relaxed);
    const QString level = peak ? QString::number(20 * log10(double(peak) / 32768.0), 'f', 1) + " dBFS" : "silence";
    return QString("\nMicBridge: LIVE | 48 kHz mono | %1 pkt/s avg | peak %2 | send errors %3")
        .arg(double(s.packets.load()) / seconds, 0, 'f', 0).arg(level).arg(s.errors.load());
}
