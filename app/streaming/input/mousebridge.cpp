#include "mousebridge.h"
#include <QDir>
#include <QFile>
#include <QSet>

#ifdef Q_OS_LINUX
#include <linux/input.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using KeyBits = std::array<unsigned char, (KEY_CNT + 7) / 8>;
template<size_t N> bool bit(const std::array<unsigned char, N>& bits, unsigned key) {
    return key / 8 < N && (bits[key / 8] & (1u << (key % 8)));
}
bool chord(const KeyBits& keys) {
    return (bit(keys, KEY_LEFTCTRL) || bit(keys, KEY_RIGHTCTRL)) &&
           (bit(keys, KEY_LEFTALT) || bit(keys, KEY_RIGHTALT)) &&
           (bit(keys, KEY_LEFTSHIFT) || bit(keys, KEY_RIGHTSHIFT)) && bit(keys, KEY_C);
}
bool watched(unsigned key) {
    return key == KEY_C || key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL ||
           key == KEY_LEFTALT || key == KEY_RIGHTALT ||
           key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT;
}
QString inputError(const QString& action) {
    return action + ": " + QString::fromLocal8Bit(strerror(errno));
}
bool relativeMouse(int fd) {
    std::array<unsigned char, (REL_CNT + 7) / 8> relative{};
    KeyBits keys{};
    return ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative)), relative.data()) >= 0 &&
           ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys.data()) >= 0 &&
           bit(relative, REL_X) && bit(relative, REL_Y) && bit(keys, BTN_LEFT);
}
void drain(int fd) {
    input_event events[64];
    while (read(fd, events, sizeof(events)) > 0) {}
}
}

struct MouseBridge::Impl {
    struct Keyboard { int fd; KeyBits supported{}; KeyBits pressed{}; bool dropped = false; };
    int mouse = -1, socket = -1, wake = -1;
    std::vector<Keyboard> keyboards;
    std::thread thread;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    bool captured = false, swap = false, reverse = false, dirty = false, dropped = false;
    int64_t dx = 0, dy = 0, wheel = 0;
    unsigned buttons = 0;
    QString failure;
    QString inputRoot;
    Clock::time_point lastSend = Clock::now();
    Clock::time_point statsStart = Clock::now();
    uint64_t reports = 0, packets = 0, sendErrors = 0, overruns = 0;
    uint64_t delaySamples = 0, delayTotalNs = 0, delayMaxNs = 0, pendingEventNs = 0;
    double reportHz = 0, packetHz = 0, delayAverageMs = 0, delayMaximumMs = 0;
    bool monotonicEvents = false, haveDelay = false;

    void updateStats() {
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - statsStart).count();
        if (seconds < 1.0) return;
        reportHz = reports / seconds;
        packetHz = packets / seconds;
        haveDelay = delaySamples != 0;
        delayAverageMs = haveDelay ? double(delayTotalNs) / delaySamples / 1e6 : 0;
        delayMaximumMs = double(delayMaxNs) / 1e6;
        reports = packets = delaySamples = delayTotalNs = delayMaxNs = 0;
        statsStart = now;
    }

    void noteEvent(const input_event& event) {
        if (!monotonicEvents || pendingEventNs) return;
        pendingEventNs = uint64_t(event.time.tv_sec) * 1000000000ULL +
                         uint64_t(event.time.tv_usec) * 1000ULL;
    }

    unsigned buttonState(const KeyBits& keys) const {
        unsigned state = 0;
        for (unsigned i = 0; i < 5; ++i) if (bit(keys, BTN_LEFT + i)) state |= 1u << i;
        if (swap) state = (state & ~3u) | ((state & 1u) << 1) | ((state & 2u) >> 1);
        return state;
    }

    void send() {
        int x = int(std::max<int64_t>(-32767, std::min<int64_t>(32767, dx)));
        int y = int(std::max<int64_t>(-32767, std::min<int64_t>(32767, dy)));
        int w = int(std::max<int64_t>(-127, std::min<int64_t>(127, wheel)));
        char packet[128];
        int length = snprintf(packet, sizeof(packet), "{\"dx\":%d,\"dy\":%d,\"wheel\":%d,\"buttons\":%u}", x, y, w, buttons);
        if (::send(socket, packet, length, MSG_DONTWAIT | MSG_NOSIGNAL) == length) {
            ++packets;
            if (pendingEventNs) {
                timespec now{};
                if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                    const uint64_t nowNs = uint64_t(now.tv_sec) * 1000000000ULL + now.tv_nsec;
                    if (nowNs >= pendingEventNs) {
                        const auto delay = nowNs - pendingEventNs;
                        ++delaySamples;
                        delayTotalNs += delay;
                        delayMaxNs = std::max(delayMaxNs, delay);
                    }
                }
            }
        }
        else ++sendErrors;
        dx -= x; dy -= y; wheel -= w;
        dirty = dx || dy || wheel;
        if (!dirty) pendingEventNs = 0;
        lastSend = Clock::now();
    }

    void release() {
        dx = dy = wheel = 0;
        buttons = 0;
        dirty = false;
        pendingEventNs = 0;
        if (socket >= 0) send();
        if (captured) ioctl(mouse, EVIOCGRAB, 0);
        captured = false;
    }

    void post(int code) {
        SDL_Event event{};
        event.type = MouseBridge::eventType();
        event.user.code = code;
        event.user.data1 = this;
        SDL_PushEvent(&event);
    }

    void fail(const QString& message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            failure = message;
            release();
        }
        post(1);
    }

    void run() {
        std::vector<pollfd> fds{{wake, POLLIN, 0}, {mouse, POLLIN, 0}};
        for (const auto& keyboard : keyboards) fds.push_back({keyboard.fd, POLLIN, 0});
        while (!stopping) {
            if (poll(fds.data(), fds.size(), 1) < 0) {
                if (errno == EINTR) continue;
                fail(inputError("MouseBridge input polling failed"));
                return;
            }
            if (fds[0].revents) break;
            if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                fail("MouseBridge mouse disconnected. Reconnect it and restart the stream.");
                return;
            }
            if (fds[1].revents & POLLIN) {
                std::lock_guard<std::mutex> lock(mutex);
                input_event events[64];
                ssize_t bytes;
                while ((bytes = read(mouse, events, sizeof(events))) > 0) {
                    if (!captured) continue;
                    for (ssize_t i = 0; i < bytes / ssize_t(sizeof(input_event)); ++i) {
                        const auto& event = events[i];
                        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
                            ++overruns;
                            pendingEventNs = 0;
                            dropped = true;
                            dx = dy = wheel = 0;
                        }
                        else if (dropped) {
                            if (event.type == EV_SYN && event.code == SYN_REPORT) {
                                KeyBits keys{};
                                ioctl(mouse, EVIOCGKEY(sizeof(keys)), keys.data());
                                buttons = buttonState(keys);
                                dirty = true;
                                dropped = false;
                            }
                        }
                        else if (event.type == EV_SYN && event.code == SYN_REPORT) {
                            ++reports;
                        }
                        else if (event.type == EV_REL) {
                            if (event.code != REL_X && event.code != REL_Y && event.code != REL_WHEEL) continue;
                            noteEvent(event);
                            if (event.code == REL_X) dx += event.value;
                            if (event.code == REL_Y) dy += event.value;
                            if (event.code == REL_WHEEL) wheel += reverse ? -event.value : event.value;
                            dirty = true;
                        }
                        else if (event.type == EV_KEY && event.code >= BTN_LEFT && event.code <= BTN_EXTRA) {
                            noteEvent(event);
                            unsigned index = event.code - BTN_LEFT;
                            if (swap && index < 2) index ^= 1;
                            if (event.value) buttons |= 1u << index;
                            else buttons &= ~(1u << index);
                            // Preserve press/release pairs even within one motion batch.
                            send();
                        }
                    }
                }
            }
            for (size_t k = 0; k < keyboards.size(); ++k) {
                if (fds[k + 2].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    close(keyboards[k].fd);
                    keyboards[k].fd = fds[k + 2].fd = -1;
                    keyboards[k].pressed.fill(0);
                    keyboards[k].supported.fill(0);
                    KeyBits remaining{};
                    for (const auto& kb : keyboards)
                        for (size_t b = 0; b < remaining.size(); ++b) remaining[b] |= kb.supported[b];
                    if (!chord(remaining)) {
                        fail("MouseBridge shortcut keyboard disconnected. Restart the stream after reconnecting it.");
                        return;
                    }
                    continue;
                }
                if (!(fds[k + 2].revents & POLLIN)) continue;
                auto& keyboard = keyboards[k];
                input_event events[64];
                ssize_t bytes;
                while ((bytes = read(keyboard.fd, events, sizeof(events))) > 0) {
                    for (ssize_t i = 0; i < bytes / ssize_t(sizeof(input_event)); ++i) {
                        const auto& event = events[i];
                        if (event.type == EV_SYN && event.code == SYN_DROPPED) keyboard.dropped = true;
                        if (keyboard.dropped) {
                            if (event.type == EV_SYN && event.code == SYN_REPORT) {
                                ioctl(keyboard.fd, EVIOCGKEY(sizeof(keyboard.pressed)), keyboard.pressed.data());
                                keyboard.dropped = false;
                            }
                            continue;
                        }
                        if (event.type != EV_KEY || !watched(event.code)) continue;
                        unsigned mask = 1u << (event.code % 8);
                        if (event.value) keyboard.pressed[event.code / 8] |= mask;
                        else keyboard.pressed[event.code / 8] &= ~mask;
                        if (event.code == KEY_C && event.value == 1) {
                            KeyBits combined{};
                            for (const auto& kb : keyboards)
                                for (size_t b = 0; b < combined.size(); ++b) combined[b] |= kb.pressed[b];
                            if (chord(combined)) post(0);
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            auto elapsed = Clock::now() - lastSend;
            if ((dirty && elapsed >= std::chrono::milliseconds(1)) || elapsed >= std::chrono::milliseconds(50)) send();
            updateStats();
        }
    }
};
#else
struct MouseBridge::Impl {};
#endif

Uint32 MouseBridge::eventType() {
    static const Uint32 type = [] {
        auto event = SDL_RegisterEvents(1);
        // Moonlight already uses SDL_USEREVENT without SDL_RegisterEvents().
        return event == SDL_USEREVENT ? SDL_RegisterEvents(1) : event;
    }();
    return type;
}

MouseBridge::MouseBridge(const QString& inputRoot) : m_Impl(new Impl) {
#ifdef Q_OS_LINUX
    m_Impl->inputRoot = inputRoot;
#else
    Q_UNUSED(inputRoot);
#endif
}

MouseBridge::~MouseBridge() {
#ifdef Q_OS_LINUX
    m_Impl->stopping = true;
    if (m_Impl->wake >= 0) { uint64_t value = 1; ::write(m_Impl->wake, &value, sizeof(value)); }
    if (m_Impl->thread.joinable()) m_Impl->thread.join();
    m_Impl->release();
    for (auto& keyboard : m_Impl->keyboards) if (keyboard.fd >= 0) close(keyboard.fd);
    if (m_Impl->mouse >= 0) close(m_Impl->mouse);
    if (m_Impl->socket >= 0) close(m_Impl->socket);
    if (m_Impl->wake >= 0) close(m_Impl->wake);
    SDL_FlushEvent(eventType());
#endif
}

QVariantList MouseBridge::devices(const QString& inputRoot) {
    QVariantList devices;
#ifdef Q_OS_LINUX
    // Prefer stable device IDs over event numbers, which change after reboot.
    QStringList paths;
    for (const auto& entry : QDir(inputRoot + "/by-id").entryInfoList({"*-event-mouse"}, QDir::Files | QDir::System)) paths << entry.absoluteFilePath();
    for (const auto& entry : QDir(inputRoot).entryInfoList({"event*"}, QDir::System | QDir::Files)) paths << entry.absoluteFilePath();
    QSet<QString> seen;
    for (const auto& path : paths) {
        const QString real = QFileInfo(path).canonicalFilePath();
        if (seen.contains(real)) continue;
        int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (relativeMouse(fd)) {
            char name[256] = {};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            devices << QVariantMap{{"name", QString::fromLocal8Bit(name) + " (" + path + ")"}, {"path", path}};
            seen.insert(real);
        }
        close(fd);
    }
#else
    Q_UNUSED(inputRoot);
#endif
    return devices;
}

bool MouseBridge::initialize(const QString& device, const QString& host, bool swapButtons,
                             bool reverseScroll, QString& error, quint16 port) {
#ifdef Q_OS_LINUX
    if (eventType() == Uint32(-1)) { error = "Cannot register MouseBridge input events."; return false; }
    if (device.isEmpty()) { error = "Select a MouseBridge mouse in Input Settings first."; return false; }
    m_Impl->mouse = open(QFile::encodeName(device).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (m_Impl->mouse < 0) { error = inputError("Cannot open MouseBridge mouse " + device); return false; }
    if (!relativeMouse(m_Impl->mouse)) { error = "The selected MouseBridge device is not a relative mouse."; return false; }
    int eventClock = CLOCK_MONOTONIC;
    m_Impl->monotonicEvents = ioctl(m_Impl->mouse, EVIOCSCLOCKID, &eventClock) == 0;
    KeyBits supported{};
    for (const auto& entry : QDir(m_Impl->inputRoot).entryInfoList({"event*"}, QDir::System | QDir::Files)) {
        if (entry.canonicalFilePath() == QFileInfo(device).canonicalFilePath()) continue;
        Impl::Keyboard keyboard;
        keyboard.fd = open(QFile::encodeName(entry.absoluteFilePath()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (keyboard.fd < 0) continue;
        ioctl(keyboard.fd, EVIOCGBIT(EV_KEY, sizeof(keyboard.supported)), keyboard.supported.data());
        bool relevant = false;
        for (unsigned key = 0; key < KEY_CNT; ++key) if (watched(key) && bit(keyboard.supported, key)) relevant = true;
        if (!relevant) { close(keyboard.fd); continue; }
        ioctl(keyboard.fd, EVIOCGKEY(sizeof(keyboard.pressed)), keyboard.pressed.data());
        for (size_t b = 0; b < supported.size(); ++b) supported[b] |= keyboard.supported[b];
        m_Impl->keyboards.push_back(keyboard);
    }
    if (!chord(supported)) { error = "MouseBridge cannot read the keyboard shortcut. Check /dev/input mouse and keyboard permissions."; return false; }
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    const QByteArray service = QByteArray::number(port);
    int result = getaddrinfo(host.toUtf8().constData(), service.constData(), &hints, &addresses);
    if (result) { error = "MouseBridge needs an IPv4 receiver address: " + QString::fromLocal8Bit(gai_strerror(result)); return false; }
    for (auto address = addresses; address; address = address->ai_next) {
        m_Impl->socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (m_Impl->socket >= 0 && ::connect(m_Impl->socket, address->ai_addr, address->ai_addrlen) == 0) break;
        if (m_Impl->socket >= 0) close(m_Impl->socket);
        m_Impl->socket = -1;
    }
    freeaddrinfo(addresses);
    if (m_Impl->socket < 0) { error = inputError("Cannot create MouseBridge socket"); return false; }
    m_Impl->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_Impl->wake < 0) { error = inputError("Cannot create MouseBridge wake event"); return false; }
    m_Impl->swap = swapButtons;
    m_Impl->reverse = reverseScroll;
    m_Impl->thread = std::thread([this] { m_Impl->run(); });
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "MouseBridge ready: %s -> %s:%u", qPrintable(device), qPrintable(host), unsigned(port));
    return true;
#else
    Q_UNUSED(device); Q_UNUSED(host); Q_UNUSED(swapButtons); Q_UNUSED(reverseScroll); Q_UNUSED(port);
    error = "MouseBridge raw input is available on Linux only.";
    return false;
#endif
}

bool MouseBridge::setCaptured(bool active, QString& error) {
#ifdef Q_OS_LINUX
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    if (!active) { m_Impl->release(); return true; }
    if (!m_Impl->failure.isEmpty()) { error = m_Impl->failure; return false; }
    if (m_Impl->captured) return true;
    if (ioctl(m_Impl->mouse, EVIOCGRAB, 1) < 0) { error = inputError("Cannot capture MouseBridge mouse"); return false; }
    drain(m_Impl->mouse);
    m_Impl->captured = true;
    m_Impl->dropped = false;
    KeyBits keys{};
    ioctl(m_Impl->mouse, EVIOCGKEY(sizeof(keys)), keys.data());
    m_Impl->buttons = m_Impl->buttonState(keys);
    m_Impl->send();
    return true;
#else
    Q_UNUSED(active); Q_UNUSED(error);
    return false;
#endif
}

bool MouseBridge::isCaptured() const {
#ifdef Q_OS_LINUX
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->captured;
#else
    return false;
#endif
}

QString MouseBridge::error() const {
#ifdef Q_OS_LINUX
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    return m_Impl->failure;
#else
    return {};
#endif
}

QString MouseBridge::statsText() const {
#ifdef Q_OS_LINUX
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    const auto& s = *m_Impl;
    const QString delay = s.haveDelay
        ? QString("avg %1 ms | max %2 ms").arg(s.delayAverageMs, 0, 'f', 2).arg(s.delayMaximumMs, 0, 'f', 2)
        : QStringLiteral("n/a (no timestamped input in last second)");
    return QString("\nMouseBridge: %1 | input %2 Hz | UDP %3 pkt/s\nMouse local event -> UDP: %4\nMouse send errors: %5 | input overruns: %6")
        .arg(s.captured ? QStringLiteral("captured") : QStringLiteral("released"))
        .arg(s.reportHz, 0, 'f', 0).arg(s.packetHz, 0, 'f', 0).arg(delay)
        .arg(qulonglong(s.sendErrors)).arg(qulonglong(s.overruns));
#else
    return {};
#endif
}

bool MouseBridge::ownsEvent(const SDL_Event& event) const {
    return event.type == eventType() && event.user.data1 == m_Impl.get();
}
