#pragma once
#include <QString>
#include <QVariantList>
#include <memory>

class MicBridge {
public:
    MicBridge();
    ~MicBridge();
    static QVariantList devices();
    bool start(const QString& device, const QString& host, QString& error);
    bool ownsDevice(unsigned id) const;
    QString statsText() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
