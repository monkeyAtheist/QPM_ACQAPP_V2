#include "qacq/app/serial_port_list.h"

#include <QSerialPortInfo>
#include <QStringList>

namespace qacq::app
{
SerialPortList::SerialPortList(QObject *parent, int refreshMilliseconds) : QAbstractListModel(parent)
{
    if (refreshMilliseconds > 0)
    {
        connect(&m_timer, &QTimer::timeout, this, [this] { refresh(); });
        m_timer.start(refreshMilliseconds);
        refresh();
    }
}

int SerialPortList::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant SerialPortList::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const Port &p = m_ports.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case LabelRole: return p.label;
        case PortNameRole: return p.name;
        case DescriptionRole: return p.description;
        case ManufacturerRole: return p.manufacturer;
        case SerialNumberRole: return p.serialNumber;
        case VendorIdRole: return static_cast<int>(p.vendorId);
        case ProductIdRole: return static_cast<int>(p.productId);
        case BoardRole: return p.board;
        case LocationRole: return p.location;
        default: return {};
    }
}

QHash<int, QByteArray> SerialPortList::roleNames() const
{
    return {
        {PortNameRole, "portName"}, {DescriptionRole, "description"}, {ManufacturerRole, "manufacturer"},
        {SerialNumberRole, "serialNumber"}, {VendorIdRole, "vendorId"}, {ProductIdRole, "productId"},
        {BoardRole, "board"}, {LabelRole, "label"}, {LocationRole, "location"},
    };
}

QString SerialPortList::boardName(quint16 vendorId, quint16 productId)
{
    switch (vendorId)
    {
        case 0x2341:
        case 0x2A03: return QStringLiteral("Arduino");
        case 0x1A86: return QStringLiteral("CH340 (carte compatible Arduino, ESP8266…)");
        case 0x10C4: return QStringLiteral("CP210x (ESP32, NodeMCU…)");
        case 0x0403: return QStringLiteral("FTDI");
        case 0x0483: return productId == 0x374B || productId == 0x374E || productId == 0x3752 ? QStringLiteral("STM32 (ST-Link)")
                                                                                              : QStringLiteral("STM32");
        case 0x303A: return QStringLiteral("ESP32 (USB natif)");
        case 0x2E8A: return QStringLiteral("Raspberry Pi Pico");
        case 0x239A: return QStringLiteral("Adafruit");
        case 0x16C0: return QStringLiteral("Teensy");
        case 0x1366: return QStringLiteral("SEGGER J-Link");
        case 0x04D8: return QStringLiteral("Microchip");
        case 0x0D28: return QStringLiteral("DAPLink (micro:bit…)");
        case 0x1915: return QStringLiteral("Nordic nRF");
        case 0x067B: return QStringLiteral("Prolific PL2303");
        default: return {};
    }
}

QString SerialPortList::labelOf(const Port &p)
{
    QString detail = !p.board.isEmpty() ? p.board : p.description;
    if (p.vendorId != 0)
        detail += QStringLiteral(" (%1:%2)")
                      .arg(p.vendorId, 4, 16, QLatin1Char('0'))
                      .arg(p.productId, 4, 16, QLatin1Char('0'));
    return detail.trimmed().isEmpty() ? p.name : p.name + QStringLiteral(" — ") + detail.trimmed();
}

void SerialPortList::setPorts(const QList<Port> &ports)
{
    QStringList before;
    for (const Port &p : m_ports) before << p.name;
    QList<Port> next = ports;
    for (Port &p : next)
    {
        if (p.board.isEmpty()) p.board = boardName(p.vendorId, p.productId);
        p.label = labelOf(p);
    }
    QStringList after;
    for (const Port &p : next) after << p.name;
    bool same = before == after;
    for (qsizetype i = 0; same && i < next.size(); ++i) same = next.at(i).label == m_ports.at(i).label;
    if (same && !m_first) return;
    const int oldCount = count();
    beginResetModel();
    m_ports = next;
    endResetModel();
    if (count() != oldCount) emit countChanged();
    emit portsChanged();
    if (!m_first)
        for (const QString &name : after)
            if (!before.contains(name)) emit portAdded(name);
    m_first = false;
}

void SerialPortList::refresh()
{
    QList<Port> ports;
    const QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos)
    {
        Port p;
        p.name = info.portName();
        p.description = info.description();
        p.manufacturer = info.manufacturer();
        p.serialNumber = info.serialNumber();
        p.location = info.systemLocation();
        p.vendorId = info.hasVendorIdentifier() ? info.vendorIdentifier() : 0;
        p.productId = info.hasProductIdentifier() ? info.productIdentifier() : 0;
        ports << p;
    }
    setPorts(ports);
}

QString SerialPortList::portAt(int row) const
{
    return row >= 0 && row < count() ? m_ports.at(row).name : QString();
}

int SerialPortList::rowOf(const QString &portName) const
{
    for (int i = 0; i < count(); ++i)
        if (m_ports.at(i).name == portName) return i;
    return -1;
}
} // namespace qacq::app
