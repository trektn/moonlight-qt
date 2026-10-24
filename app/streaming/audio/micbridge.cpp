#include "micbridge.h"
#include <SDL.h>
#include <QRandomGenerator>
#include <QtEndian>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <array>
#include <mutex>
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
    std::atomic<bool> live{false};
    std::atomic<Uint64> lastCapture{0};
    Uint64 started = 0;
    mutable std::mutex statsMutex;
    QString failure;
    struct Stats {
        double sendHz = 0, payloadKbps = 0, rmsDb = -120, peakDb = -120, clipping = 0;
        double ackMs = 0, queueMs = 0, paddingMs = 0, bufferMs = 0;
        Uint64 feedbackAt = 0;
        quint64 received = 0, missing = 0, late = 0, underruns = 0, trims = 0;
        bool playing = false;
    } stats;
    struct Stamp { quint32 sequence = 0; Uint64 at = 0; };
    std::array<Stamp, 1024> history{};
    Uint64 windowStart = 0;
    quint64 windowSamples = 0, windowSquares = 0, windowClipped = 0, previousPackets = 0;
    int windowPeak = 0;
    double captureBufferMs = 0;

    void feedback() {
#ifdef Q_OS_LINUX
        for (int batch = 0; batch < 8; ++batch) {
            unsigned char report[256];
            const auto bytes = recv(socket, report, sizeof(report), MSG_DONTWAIT);
            if (bytes < 0) break;
            if (bytes != 72 || memcmp(report, "MCS1", 4) || qFromBigEndian<quint32>(report + 4) != session) continue;
            const quint32 acknowledged = qFromBigEndian<quint32>(report + 8);
            const auto stamp = history[acknowledged % history.size()];
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.feedbackAt = SDL_GetTicks64();
            stats.ackMs = stamp.at && stamp.sequence == acknowledged
                ? double(SDL_GetPerformanceCounter() - stamp.at) * 1000.0 / SDL_GetPerformanceFrequency() : -1;
            stats.queueMs = qFromBigEndian<quint32>(report + 12) / 48.0;
            stats.paddingMs = qFromBigEndian<quint32>(report + 16) / 48.0;
            stats.bufferMs = qFromBigEndian<quint32>(report + 20) / 48.0;
            stats.received = qFromBigEndian<quint64>(report + 24);
            stats.missing = qFromBigEndian<quint64>(report + 32);
            stats.late = qFromBigEndian<quint64>(report + 40);
            stats.underruns = qFromBigEndian<quint64>(report + 48);
            stats.trims = qFromBigEndian<quint64>(report + 56);
            stats.playing = qFromBigEndian<quint32>(report + 68) != 0;
        }
#endif
    }

    void updateStats(Uint64 now) {
        if (now - windowStart < 1000) return;
        const auto sent = packets.load();
        const double seconds = (now - windowStart) / 1000.0;
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.sendHz = double(sent - previousPackets) / seconds;
            stats.payloadKbps = stats.sendHz * 496 * 8 / 1000.0;
            stats.peakDb = windowPeak ? 20 * log10(windowPeak / 32768.0) : -120;
            stats.rmsDb = windowSquares && windowSamples
                ? 10 * log10(double(windowSquares) / windowSamples / (32768.0 * 32768.0)) : -120;
            stats.clipping = windowSamples ? 100.0 * windowClipped / windowSamples : 0;
        }
        previousPackets = sent; windowStart = now;
        windowSamples = windowSquares = windowClipped = 0; windowPeak = 0;
    }

    void send(bool end = false) {
#ifdef Q_OS_LINUX
        memcpy(packet, "MCB1", 4);
        qToBigEndian(session, packet + 4);
        const auto sentSequence = sequence++;
        qToBigEndian(sentSequence, packet + 8);
        qToBigEndian<quint16>(end ? 1 : 0, packet + 12);
        qToBigEndian<quint16>(end ? 0 : 240, packet + 14);
        const int bytes = end ? 16 : sizeof(packet);
        const auto at = SDL_GetPerformanceCounter();
        if (::send(socket, packet, bytes, MSG_DONTWAIT | MSG_NOSIGNAL) == bytes) {
            ++packets;
            history[sentSequence % history.size()] = {sentSequence, at};
        }
        else ++errors;
#else
        Q_UNUSED(end);
#endif
    }
    static void capture(void* context, Uint8* data, int length) {
        auto& self = *static_cast<Impl*>(context);
        int peak = 0;
        for (int i = 0; i + 1 < length; i += 2) {
            const int sample = int(qFromLittleEndian<qint16>(data + i));
            peak = std::max(peak, std::abs(sample));
            self.windowSquares += quint64(qint64(sample) * sample);
            self.windowClipped += std::abs(sample) >= 32767;
        }
        self.windowSamples += unsigned(length) / 2;
        self.windowPeak = std::max(self.windowPeak, peak);
        while (length > 0) {
            const unsigned bytes = std::min(unsigned(length), 480u - self.used);
            memcpy(self.packet + 16 + self.used, data, bytes);
            self.used += bytes; data += bytes; length -= int(bytes);
            if (self.used == 480) { self.send(); self.used = 0; }
        }
        const auto now = SDL_GetTicks64();
        self.lastCapture.store(now, std::memory_order_relaxed);
        self.feedback();
        self.updateStats(now);
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
    const auto fail = [&](const QString& reason) {
        error = reason;
        std::lock_guard<std::mutex> lock(m_Impl->statsMutex);
        m_Impl->failure = reason;
        return false;
    };
#ifdef Q_OS_LINUX
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    int status = getaddrinfo(host.toUtf8().constData(), "39842", &hints, &addresses);
    if (status) return fail("MicBridge receiver address: " + QString::fromLocal8Bit(gai_strerror(status)));
    for (auto addr = addresses; addr; addr = addr->ai_next) {
        m_Impl->socket = ::socket(addr->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (m_Impl->socket >= 0 && ::connect(m_Impl->socket, addr->ai_addr, addr->ai_addrlen) == 0) break;
        if (m_Impl->socket >= 0) close(m_Impl->socket);
        m_Impl->socket = -1;
    }
    freeaddrinfo(addresses);
    if (m_Impl->socket < 0) return fail("MicBridge cannot open UDP socket.");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return fail(QString::fromUtf8(SDL_GetError()));
    m_Impl->audioInitialized = true;
    // SDL2-compat's named-device IDs become stale across SDL audio shutdowns.
    // Settings enumeration and decoder probing both cycle the audio subsystem.
    // Refresh IDs after initialization, immediately before opening capture.
    bool found = device.isEmpty();
    for (int i = 0; i < SDL_GetNumAudioDevices(1); ++i) {
        const char* name = SDL_GetAudioDeviceName(i, 1);
        if (name && device == QString::fromUtf8(name)) found = true;
    }
    if (!found) return fail("MicBridge microphone is unavailable: " + device);
    SDL_AudioSpec wanted{}, obtained{};
    wanted.freq = 48000; wanted.channels = 1; wanted.format = AUDIO_S16LSB;
    wanted.samples = 240; wanted.callback = Impl::capture; wanted.userdata = m_Impl.get();
    const QByteArray name = device.toUtf8();
    m_Impl->device = SDL_OpenAudioDevice(device.isEmpty() ? nullptr : name.constData(), 1, &wanted, &obtained, 0);
    if (!m_Impl->device) return fail("MicBridge microphone: " + QString::fromUtf8(SDL_GetError()));
    m_Impl->started = SDL_GetTicks64();
    m_Impl->windowStart = m_Impl->started;
    m_Impl->captureBufferMs = obtained.samples * 1000.0 / obtained.freq;
    m_Impl->live.store(true, std::memory_order_release);
    SDL_PauseAudioDevice(m_Impl->device, 0);
    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "MicBridge capturing %s -> %s:39842 (48 kHz mono PCM, 5 ms packets)",
                device.isEmpty() ? "default microphone" : name.constData(), qPrintable(host));
    return true;
#else
    Q_UNUSED(device); Q_UNUSED(host);
    return fail("MicBridge capture is available on Linux only.");
#endif
}

bool MicBridge::ownsDevice(unsigned id) const { return m_Impl->device == id; }

QString MicBridge::statsText() const {
    const auto& s = *m_Impl;
    Impl::Stats stats;
    QString failure;
    {
        std::lock_guard<std::mutex> lock(s.statsMutex);
        stats = s.stats; failure = s.failure;
    }
    if (!s.live.load(std::memory_order_acquire))
        return failure.isEmpty() ? QStringLiteral("\nMicBridge: starting capture") : "\nMicBridge: ERROR - " + failure;
    const auto now = SDL_GetTicks64();
    const bool connected = stats.feedbackAt && now - stats.feedbackAt < 2000;
    QString text = QString("\nMicBridge: %1 | receiver %2 | 48 kHz mono | capture buffer %3 ms\nMic level: peak %4 / RMS %5 dBFS | clipping %6%\nMic send: %7 pkt/s | %8 kbit/s payload | errors %9")
        .arg(now - s.lastCapture.load() < 2000 ? "LIVE" : "CAPTURE STALLED")
        .arg(connected ? (stats.playing ? "playing" : "buffering") : "NO FEEDBACK")
        .arg(s.captureBufferMs, 0, 'f', 1).arg(stats.peakDb, 0, 'f', 1).arg(stats.rmsDb, 0, 'f', 1)
        .arg(stats.clipping, 0, 'f', 2).arg(stats.sendHz, 0, 'f', 0).arg(stats.payloadKbps, 0, 'f', 1).arg(s.errors.load());
    if (!connected) return text + "\nMic receiver: no recent acknowledgement (requires receiver 1.1+)";
    const QString ack = stats.ackMs >= 0 ? QString::number(stats.ackMs, 'f', 2) + " ms" : "n/a";
    return text + QString("\nMic feedback RTT: %1 | queued %2 ms | output padding %3 ms (buffer %4 ms)\nMic receiver: %5 packets | missing %6 | late %7 | underruns %8 | trims %9")
        .arg(ack).arg(stats.queueMs, 0, 'f', 1).arg(stats.paddingMs, 0, 'f', 1).arg(stats.bufferMs, 0, 'f', 1)
        .arg(stats.received).arg(stats.missing).arg(stats.late).arg(stats.underruns).arg(stats.trims);
}
