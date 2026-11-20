#pragma once

// The serial ports of the PC as a list model for the interface, refreshed every 1.5 s (a board plugged
// in appears by itself), with the kind of board guessed from the USB identifiers: Arduino, CH340, CP210x,
// FTDI, STM32, ESP32, Raspberry Pi Pico...

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QTimer>

namespace qacq::app
{
class SerialPortList : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role
    {
        PortNameRole = Qt::UserRole + 1, // "COM3"
        DescriptionRole,                 // as the driver says
        ManufacturerRole,
        SerialNumberRole,
        VendorIdRole,  // 0 when unknown
        ProductIdRole,
        BoardRole,     // "Arduino", "STM32 (ST-Link)"... empty when unknown
        LabelRole,     // "COM3 — Arduino (2341:0043)"
        LocationRole   // system location ("\\\\.\\COM3", "/dev/ttyACM0")
    };
    Q_ENUM(Role)

    struct Port
    {
        QString name;
        QString description;
        QString manufacturer;
        QString serialNumber;
        QString location;
        quint16 vendorId = 0;
        quint16 productId = 0;
        QString board;
        QString label;
    };

    // refreshMilliseconds = 0: no automatic refresh (tests).
    explicit SerialPortList(QObject *parent = nullptr, int refreshMilliseconds = 1500);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_ports.size()); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString portAt(int row) const;
    Q_INVOKABLE int rowOf(const QString &portName) const;

    // Replaces the list (refresh() reads the system; tests give their own).
    void setPorts(const QList<Port> &ports);
    static QString boardName(quint16 vendorId, quint16 productId);
    static QString labelOf(const Port &port);

signals:
    void countChanged();
    void portsChanged();
    void portAdded(const QString &portName); // plugged in since the previous refresh

private:
    QList<Port> m_ports;
    QTimer m_timer;
    bool m_first = true;
};
} // namespace qacq::app
