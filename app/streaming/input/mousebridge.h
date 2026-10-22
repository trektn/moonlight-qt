#pragma once

#include <QString>
#include <QVariantList>
#include <SDL.h>
#include <memory>

// Owns raw evdev input and a dedicated 1 ms UDP sender, independent of video FPS.
class MouseBridge
{
public:
    explicit MouseBridge(const QString& inputRoot = QStringLiteral("/dev/input"));
    ~MouseBridge();

    static QVariantList devices(const QString& inputRoot = QStringLiteral("/dev/input"));
    static Uint32 eventType();
    bool initialize(const QString& device, const QString& host, bool swapButtons,
                    bool reverseScroll, QString& error, quint16 port = 39841);
    bool setCaptured(bool active, QString& error);
    bool isCaptured() const;
    QString error() const;
    bool ownsEvent(const SDL_Event& event) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
