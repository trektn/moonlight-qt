// Real polling/UDP with synthetic FIFO devices; never opens or grabs real input.
#include "../../app/streaming/input/mousebridge.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdarg>
#include <atomic>
#include <cstring>

static ino_t mouseInode, keyboardInode;
static std::atomic<bool> grabbed{false}, rejectGrab{false};
extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    if (request == EVIOCGRAB) {
        int active = va_arg(args, int);
        va_end(args);
        struct stat st{};
        if (fstat(fd, &st) || st.st_ino != mouseInode) return __real_ioctl(fd, request, active);
        if (active && rejectGrab) { errno = EBUSY; return -1; }
        grabbed = active;
        return 0;
    }
    void* output = va_arg(args, void*);
    va_end(args);
    struct stat st{};
    if (fstat(fd, &st) || (st.st_ino != mouseInode && st.st_ino != keyboardInode)) return __real_ioctl(fd, request, output);
    auto bytes = static_cast<unsigned char*>(output);
    memset(bytes, 0, _IOC_SIZE(request));
    auto set = [bytes, request](unsigned bit) { if (bit / 8 < _IOC_SIZE(request)) bytes[bit / 8] |= 1u << (bit % 8); };
    if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_REL, 1)) && st.st_ino == mouseInode) { set(REL_X); set(REL_Y); }
    if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_KEY, 1))) {
        if (st.st_ino == mouseInode) { set(BTN_LEFT); set(BTN_RIGHT); }
        else for (auto key : {KEY_C, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTSHIFT, KEY_RIGHTSHIFT}) set(key);
    }
    if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(1))) strncpy(static_cast<char*>(output), "Test mouse", _IOC_SIZE(request) - 1);
    return 0;
}

class BridgeTest : public QObject {
    Q_OBJECT
    QTemporaryDir root;
    int mouse = -1, keyboard = -1, receiver = -1;
    quint16 port = 0;

    void input(int fd, unsigned type, unsigned code, int value) {
        input_event event{};
        event.type = type; event.code = code; event.value = value;
        QCOMPARE(::write(fd, &event, sizeof(event)), ssize_t(sizeof(event)));
    }
    QList<QJsonObject> packets(int duration = 30) {
        QList<QJsonObject> result;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < duration) {
            pollfd fd{receiver, POLLIN, 0};
            if (::poll(&fd, 1, 2) > 0) {
                char data[256];
                auto length = ::recv(receiver, data, sizeof(data), 0);
                if (length > 0) result << QJsonDocument::fromJson(QByteArray(data, length)).object();
            }
        }
        return result;
    }
    bool toggleEvent(MouseBridge& bridge, int code = 0) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) if (bridge.ownsEvent(event) && event.user.code == code) return true;
        return false;
    }
    void key(unsigned code, int value) { input(keyboard, EV_KEY, code, value); }
    QString mousePath() const { return root.path() + "/event0"; }

private slots:
    void initTestCase() { QCOMPARE(SDL_Init(SDL_INIT_EVENTS), 0); }
    void cleanupTestCase() { SDL_Quit(); }
    void init() {
        grabbed = rejectGrab = false;
        auto path = QFile::encodeName(mousePath());
        ::unlink(path.constData());
        QCOMPARE(mkfifo(path.constData(), 0600), 0);
        mouse = open(path.constData(), O_RDWR | O_NONBLOCK);
        struct stat st{}; fstat(mouse, &st); mouseInode = st.st_ino;
        path = QFile::encodeName(root.path() + "/event1");
        ::unlink(path.constData());
        QCOMPARE(mkfifo(path.constData(), 0600), 0);
        keyboard = open(path.constData(), O_RDWR | O_NONBLOCK);
        fstat(keyboard, &st); keyboardInode = st.st_ino;
        receiver = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        QCOMPARE(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        socklen_t size = sizeof(address); getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
    }
    void cleanup() {
        if (mouse >= 0) close(mouse);
        if (keyboard >= 0) close(keyboard);
        if (receiver >= 0) close(receiver);
    }
    void enumerateAndRejectInvalidDevice() {
        auto list = MouseBridge::devices(root.path());
        QCOMPARE(list.size(), 1);
        QCOMPARE(list[0].toMap()["path"].toString(), mousePath());
        MouseBridge bridge(root.path());
        QString error;
        QVERIFY(!bridge.initialize(root.path() + "/missing", "127.0.0.1", false, false, error, port));
        QVERIFY(!error.isEmpty());
        QVERIFY(!grabbed);
    }
    void pauseResumeAndGlobalShortcut() {
        MouseBridge bridge(root.path());
        QString error;
        QVERIFY2(bridge.initialize(mousePath(), "127.0.0.1", false, false, error, port), qPrintable(error));
        QVERIFY(bridge.setCaptured(true, error));
        QVERIFY(grabbed);
        packets();
        input(mouse, EV_REL, REL_X, 17);
        bool moved = false;
        for (const auto& packet : packets()) if (packet["dx"].toInt() == 17) moved = true;
        QVERIFY(moved);

        for (auto code : {KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_RIGHTSHIFT, KEY_C}) key(code, 1);
        bool toggled = false;
        QTRY_VERIFY_WITH_TIMEOUT((toggled = toggled || toggleEvent(bridge)), 500);
        QVERIFY(bridge.setCaptured(false, error));
        QVERIFY(!grabbed);
        for (const auto& packet : packets()) QCOMPARE(packet["buttons"].toInt(), 0);
        key(KEY_C, 2); // Auto-repeat must not request another toggle.
        QTest::qWait(20);
        QVERIFY(!toggleEvent(bridge));
        input(mouse, EV_REL, REL_X, 99);
        for (const auto& packet : packets()) QCOMPARE(packet["dx"].toInt(), 0);
        key(KEY_C, 0); key(KEY_C, 1);
        toggled = false;
        QTRY_VERIFY_WITH_TIMEOUT((toggled = toggled || toggleEvent(bridge)), 500);
        // Queue paused movement just before regrab; it must be discarded.
        input(mouse, EV_REL, REL_X, 123);
        QVERIFY(bridge.setCaptured(true, error));
        packets();
        input(mouse, EV_REL, REL_X, 7);
        moved = false;
        for (const auto& packet : packets()) {
            QVERIFY(packet["dx"].toInt() != 99 && packet["dx"].toInt() != 123);
            if (packet["dx"].toInt() == 7) moved = true;
        }
        QVERIFY(moved);
    }
    void preserveButtonTransitionsAndDisconnectRelease() {
        MouseBridge bridge(root.path());
        QString error;
        QVERIFY(bridge.initialize(mousePath(), "127.0.0.1", true, false, error, port));
        QVERIFY(bridge.setCaptured(true, error));
        packets();
        input(mouse, EV_KEY, BTN_LEFT, 1);
        input(mouse, EV_KEY, BTN_LEFT, 0);
        auto received = packets();
        QVERIFY(received.size() >= 2);
        QCOMPARE(received[0]["buttons"].toInt(), 2); // swapped left/right
        QCOMPARE(received[1]["buttons"].toInt(), 0);
        close(keyboard); keyboard = -1;
        bool failed = false;
        QTRY_VERIFY_WITH_TIMEOUT((failed = failed || toggleEvent(bridge, 1)), 500);
        QVERIFY(!bridge.isCaptured());
        QVERIFY(!grabbed);
        QVERIFY(bridge.error().contains("disconnected"));
    }
    void regrabFailureLeavesMouseFree() {
        MouseBridge bridge(root.path());
        QString error;
        QVERIFY(bridge.initialize(mousePath(), "127.0.0.1", false, false, error, port));
        QVERIFY(bridge.setCaptured(true, error));
        QVERIFY(bridge.setCaptured(false, error));
        rejectGrab = true;
        QVERIFY(!bridge.setCaptured(true, error));
        QVERIFY(!bridge.isCaptured());
        QVERIFY(!grabbed);
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(BridgeTest)
#include "test.moc"
