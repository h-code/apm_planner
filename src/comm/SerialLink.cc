/*=====================================================================
======================================================================*/
/**
 * @file
 *   @brief Cross-platform support for serial ports
 *
 *   @author Lorenz Meier <mavteam@student.ethz.ch>
 *
 */

#include "QsLog.h"
#include "SerialLink.h"
#include "LinkManager.h"
#include "QGC.h"
#include "UASInterface.h"

#include <QTimer>

#include <QSettings>
#include <QMutexLocker>
//#include <QtSerialPort/QSerialPort>
//#include <QtSerialPort/QSerialPortInfo>

#include <qserialport.h>
#include <qserialportinfo.h>
#include <MG.h>

SerialLink::SerialLink() :
    m_bytesRead(0),
    m_port(NULL),
    m_baud(QSerialPort::Baud115200),
    m_dataBits(QSerialPort::Data8),
    m_flowControl(QSerialPort::NoFlowControl),
    m_stopBits(QSerialPort::OneStop),
    m_parity(QSerialPort::NoParity),
    m_portName(""),
    m_stopp(false),
    m_reqReset(false)
{
    QLOG_INFO() << "create SerialLink: Load Previous Settings ";
    m_baud = -1;
    loadSettings();
    m_id = getNextLinkId();

    if (m_portName.length() == 0) {
        // Create a new serial link
        getCurrentPorts();
        if (!m_ports.isEmpty())
            m_portName = m_ports.first().trimmed();
        else
            m_portName = "No Devices";
    }

    QLOG_INFO() <<  m_portName << m_baud << m_flowControl
             << m_parity << m_dataBits << m_stopBits;

}
void SerialLink::requestReset()
{
    QMutexLocker locker(&this->m_stoppMutex);
    m_reqReset = true;
}

SerialLink::~SerialLink()
{
    disconnect();
    writeSettings();
    QLOG_INFO() << "Serial Link destroyed";
    if(m_port) delete m_port;
    m_port = NULL;
}

QList<QString> SerialLink::getCurrentPorts()
{
    m_ports.clear();

    QList<QSerialPortInfo> portList =  QSerialPortInfo::availablePorts();

    if( portList.count() == 0){
        QLOG_INFO() << "No Ports Found" << m_ports;
    }

    foreach (const QSerialPortInfo &info, portList)
    {
        QLOG_TRACE() << "PortName    : " << info.portName()
                     << "Description : " << info.description();
        QLOG_TRACE() << "Manufacturer: " << info.manufacturer();

        m_ports.append(info.portName());
    }
    return m_ports;
}

void SerialLink::loadSettings()
{
    // Load defaults from settings
    QSettings settings;
    settings.sync();
    if (settings.contains("SERIALLINK_COMM_PORT"))
    {
        m_portName = settings.value("SERIALLINK_COMM_PORT").toString();
        m_baud = settings.value("SERIALLINK_COMM_BAUD").toInt();
        m_parity = settings.value("SERIALLINK_COMM_PARITY").toInt();
        m_stopBits = settings.value("SERIALLINK_COMM_STOPBITS").toInt();
        m_dataBits = settings.value("SERIALLINK_COMM_DATABITS").toInt();
        m_flowControl = settings.value("SERIALLINK_COMM_FLOW_CONTROL").toInt();
        QString portbaudmap = settings.value("SERIALLINK_COMM_PORTMAP").toString();
        QStringList portbaudsplit = portbaudmap.split(",");
        foreach (QString portbaud,portbaudsplit)
        {
            if (portbaud.split(":").size() == 2)
            {
                m_portBaudMap[portbaud.split(":")[0]] = portbaud.split(":")[1].toInt();
            }
        }
        if (m_portBaudMap.size() == 0)
        {
            m_portBaudMap[m_portName] = m_baud;
        }
    }
}

void SerialLink::writeSettings()
{
    // Store settings
    QSettings settings;
    settings.setValue("SERIALLINK_COMM_PORT", getPortName());
    settings.setValue("SERIALLINK_COMM_BAUD", getBaudRateType());
    settings.setValue("SERIALLINK_COMM_PARITY", getParityType());
    settings.setValue("SERIALLINK_COMM_STOPBITS", getStopBits());
    settings.setValue("SERIALLINK_COMM_DATABITS", getDataBits());
    settings.setValue("SERIALLINK_COMM_FLOW_CONTROL", getFlowType());
    QString portbaudmap = "";
    for (QMap<QString,int>::const_iterator i=m_portBaudMap.constBegin();i!=m_portBaudMap.constEnd();i++)
    {
        portbaudmap += i.key() + ":" + QString::number(i.value()) + ",";
    }
    portbaudmap = portbaudmap.mid(0,portbaudmap.length()-1); //Remove the last comma (,)
    settings.setValue("SERIALLINK_COMM_PORTMAP",portbaudmap);
    settings.sync();
}


/**
 * @brief Runs the thread
 *
 **/
void SerialLink::run()
{
    // Initialize the connection
    if (!hardwareConnect())
    {
        //Need to error out here.
        emit communicationError(getName(),"Error connecting: " + m_port->errorString());
        disconnect(); // This tidies up and sends the necessary signals
        return;
    }

    // Qt way to make clear what a while(1) loop does
    qint64 msecs = QDateTime::currentMSecsSinceEpoch();
    qint64 initialmsecs = QDateTime::currentMSecsSinceEpoch();
    quint64 bytes = 0;
    bool triedreset = false;
    bool triedDTR = false;
    qint64 timeout = 5000;

    forever
    {
        {
            QMutexLocker locker(&this->m_stoppMutex);
            if(m_stopp)
            {
                m_stopp = false;
                break; // exit the thread
            }

            if (m_reqReset)
            {
                m_reqReset = false;
                communicationUpdate(getName(),"Reset requested via DTR signal");
                m_port->setDataTerminalReady(true);
                msleep(250);
                m_port->setDataTerminalReady(false);
            }
        }

        if (m_transmitBuffer.length() > 0) {
            QMutexLocker writeLocker(&m_writeMutex);
            int numWritten = m_port->write(m_transmitBuffer);
            bool txError = m_port->waitForBytesWritten(-1);
            if ((txError) || (numWritten == -1))
                QLOG_TRACE() << "TX Error!";
            m_transmitBuffer =  m_transmitBuffer.remove(0, numWritten);
        } else {
            QLOG_TRACE() << "Wait write response timeout %1" << QTime::currentTime().toString();
        }

        bool error = m_port->waitForReadyRead(10);

        if(error) { // Waits for 1/2 second [TODO][BB] lower to SerialLink::poll_interval?
            QByteArray readData = m_port->readAll();
            while (m_port->waitForReadyRead(10))
                readData += m_port->readAll();
            if (readData.length() > 0) {
                emit bytesReceived(this, readData);
                QLOG_TRACE() << "rx of length " << QString::number(readData.length());

                m_bytesRead += readData.length();
                m_bitsReceivedTotal += readData.length() * 8;
            }
        } else {
            QLOG_TRACE() << "Wait write response timeout %1" << QTime::currentTime().toString();
        }

        if (bytes != m_bytesRead) // i.e things are good and data is being read.
        {
            bytes = m_bytesRead;
            msecs = QDateTime::currentMSecsSinceEpoch();
        }
        else
        {
            /*
                MLC - The entire timeout code block has been disabled for the time being.
                There needs to be more discussion about when and how to do resets, as it is
                inherently unsafe that we can reset PX4 via software at any time (even in flight!!!)
                Possibly query the user to be sure?
            */
        /*

            if (QDateTime::currentMSecsSinceEpoch() - msecs > timeout)
            {
                //It's been 10 seconds since the last data came in. Reset and try again
                msecs = QDateTime::currentMSecsSinceEpoch();
                if (msecs - initialmsecs > 25000)
                {
                    //After initial 25 seconds, timeouts are increased to 30 seconds.
                    //This prevents temporary silences from things like calibration commands
                    //from screwing things up. In all reality, timeouts should be enabled/disabled
                    //for events like that on a case by case basis.
                    //TODO ^^
                    timeout = 30000;
                }
                if (!triedDTR && triedreset)
                {
                    triedDTR = true;
                    communicationUpdate(getName(),"No data to receive on COM port. Attempting to reset via DTR signal");
                    QLOG_TRACE() << "No data!!! Attempting reset via DTR.";
                    m_port->setDataTerminalReady(true);
                    msleep(250);
                    m_port->setDataTerminalReady(false);
                }
                else if (!triedreset)
                {
                    QLOG_DEBUG() << "No data!!! Attempting reset via reboot command.";
                    communicationUpdate(getName(),"No data to receive on COM port. Assuming possible terminal mode, attempting to reset via \"reboot\" command");
                    m_port->write("reboot\r\n",8);
                    triedreset = true;
                }
                else
                {
                    communicationUpdate(getName(),"No data to receive on COM port....");
                    QLOG_DEBUG() << "No data!!!";
                }
            }*/
        }
        MG::SLEEP::msleep(SerialLink::poll_interval);
    } // end of forever
    
    {
        QMutexLocker locker(&this->m_stoppMutex);
        if (m_port) { // [TODO][BB] Not sure we need to close the port here
            QLOG_DEBUG() << "Closing Port #"<< __LINE__ << m_port->portName();

            m_port->close();
            delete m_port;
            m_port = NULL;
        }
    }

    emit disconnected();
    emit connected(false);
    emit disconnected(this);

}

void SerialLink::writeBytes(const char* data, qint64 size)
{
    if(m_port && m_port->isOpen()) {
        QLOG_TRACE() << "writeBytes" << m_portName << "attempting to tx " << size << "bytes.";

        QByteArray byteArray(data, size);
        {
            QMutexLocker writeLocker(&m_writeMutex);
            m_transmitBuffer.append(byteArray);
        }

        // Increase write counter
        m_bitsSentTotal += size * 8;

        // Extra debug logging
        QLOG_TRACE() << QByteArray(data,size);
    } else {
        disconnect();
        // Error occured
        emit communicationError(getName(), tr("Could not send data - link %1 is disconnected!").arg(getName()));
    }
}

/**
 * @brief Read a number of bytes from the interface.
 *
 * @param data Pointer to the data byte array to write the bytes to
 * @param maxLength The maximum number of bytes to write
 **/
void SerialLink::readBytes()
{
    m_dataMutex.lock();
    if(m_port && m_port->isOpen()) {
        const qint64 maxLength = 2048;
        char data[maxLength];
        qint64 numBytes = m_port->bytesAvailable();
        QLOG_TRACE() << "numBytes: " << numBytes;

        if(numBytes > 0) {
            /* Read as much data in buffer as possible without overflow */
            if(maxLength < numBytes) numBytes = maxLength;

            m_port->read(data, numBytes);
            QByteArray b(data, numBytes);
            emit bytesReceived(this, b);

            QLOG_TRACE() << "SerialLink::readBytes()" << std::hex << data;
            //            int i;
            //            for (i=0; i<numBytes; i++){
            //                unsigned int v=data[i];
            //
            //                fprintf(stderr,"%02x ", v);
            //            }
            //            fprintf(stderr,"\n");
            m_bitsReceivedTotal += numBytes * 8;
        }
    }
    m_dataMutex.unlock();
}


/**
 * @brief Get the number of bytes to read.
 *
 * @return The number of bytes to read
 **/
qint64 SerialLink::bytesAvailable()
{
    QLOG_TRACE() << "Serial Link bytes available";
    if (m_port) {
        return m_port->bytesAvailable();
    } else {
        return 0;
    }
}

/**
 * @brief Disconnect the connection.
 *
 * @return True if connection has been disconnected, false if connection couldn't be disconnected.
 **/
bool SerialLink::disconnect()
{
    QLOG_INFO() << "disconnect";
    if (m_port)
        QLOG_INFO() << m_port->portName();

    if (isRunning())
    {
        QLOG_INFO() << "running so disconnect" << m_port->portName();
        {
            QMutexLocker locker(&m_stoppMutex);
            m_stopp = true;
        }
        // [TODO] these signals are also emitted from RUN()
        // are these even required?
        emit disconnected();
        emit connected(false);
        emit disconnected(this);
        return true;
    }
    // [TODO]
    // Should we emit the disconncted signals to keep the states
    // in order. ie. if disconned is called the UI maybe out of sync
    // and a emit disconnect here could rectify this
    QLOG_INFO() << "already disconnected";
    return true;
}

/**
 * @brief Connect the connection.
 *
 * @return True if connection has been established, false if connection couldn't be established.
 **/
bool SerialLink::connect()
{   
    if (isRunning())
        disconnect();
    {
        QMutexLocker locker(&this->m_stoppMutex);
        m_stopp = false;
    }

    start(LowPriority);
    return true;
}

/**
 * @brief This function is called indirectly by the connect() call.
 *
 * The connect() function starts the thread and indirectly calls this method.
 *
 * @return True if the connection could be established, false otherwise
 * @see connect() For the right function to establish the connection.
 **/
bool SerialLink::hardwareConnect()
{
    if(m_port)
    {
        QLOG_INFO() << "SerialLink:" << QString::number((long)this, 16) << "closing port";
        m_port->close();
        delete m_port;
        m_port = NULL;
    }
    QLOG_INFO() << "SerialLink: hardwareConnect to " << m_portName;
    m_port = new QSerialPort(m_portName);

    if (m_port == NULL)
    {
        emit communicationUpdate(getName(),"Error opening port: " + m_port->errorString());
        return false; // couldn't create serial port.
    }

    QObject::connect(m_port,SIGNAL(aboutToClose()),this,SIGNAL(disconnected()));
//    QObject::connect(m_port, SIGNAL(error(QSerialPort::SerialPortError)),
//                     this, SLOT(linkError(QSerialPort::SerialPortError)));

//    port->setCommTimeouts(QSerialPort::CtScheme_NonBlockingRead);
    m_connectionStartTime = MG::TIME::getGroundTimeNow();

    if (!m_port->open(QIODevice::ReadWrite))
    {
        emit communicationUpdate(getName(),"Error opening port: " + m_port->errorString());
        m_port->close();
        return false; // couldn't open serial port
    }

    emit communicationUpdate(getName(),"Opened port!");

    // Need to configure the port
    m_port->setBaudRate(m_baud);
    m_port->setDataBits(static_cast<QSerialPort::DataBits>(m_dataBits));
    m_port->setFlowControl(static_cast<QSerialPort::FlowControl>(m_flowControl));
    m_port->setStopBits(static_cast<QSerialPort::StopBits>(m_stopBits));
    m_port->setParity(static_cast<QSerialPort::Parity>(m_parity));

    emit connected();
    emit connected(true);
    emit connected(this);

    QLOG_DEBUG() << "CONNECTING LINK: "<< m_portName << "with settings" << m_port->portName()
             << getBaudRate() << getDataBits() << getParityType() << getStopBits();

    writeSettings();

    return true; // successful connection
}

void SerialLink::linkError(QSerialPort::SerialPortError error)
{
    QLOG_ERROR() << error;
}


/**
 * @brief Check if connection is active.
 *
 * @return True if link is connected, false otherwise.
 **/
bool SerialLink::isConnected()
{

    if (m_port) {
        bool isConnected = m_port->isOpen();
        QLOG_TRACE() << "SerialLink #" << __LINE__ << ":"<<  m_port->portName()
                     << " isConnected =" << QString::number(isConnected);
        return isConnected;
    } else {
        QLOG_TRACE() << "SerialLink #" << __LINE__ << ":" <<  m_portName
                     << " isConnected = NULL";
        return false;
    }
}

int SerialLink::getId()
{
    return m_id;
}

QString SerialLink::getName()
{
    return m_portName;
}

/**
  * This function maps baud rate constants to numerical equivalents.
  * It relies on the mapping given in qportsettings.h from the QSerialPort library.
  */
qint64 SerialLink::getNominalDataRate()
{
    int baudRate;
    if (m_port) {
        int newBaud = m_port->baudRate();
        if (newBaud!=0)
            baudRate = newBaud;
    } else {
        baudRate = m_baud;
    }
    QLOG_DEBUG() << "getNominalDataRate() :" << baudRate;
    qint64 dataRate;
    switch (baudRate)
    {
        case QSerialPort::Baud1200:
            dataRate = 1200;
            break;
        case QSerialPort::Baud2400:
            dataRate = 2400;
            break;
        case QSerialPort::Baud4800:
            dataRate = 4800;
            break;
        case QSerialPort::Baud9600:
            dataRate = 9600;
            break;
        case QSerialPort::Baud19200:
            dataRate = 19200;
            break;
        case QSerialPort::Baud38400:
            dataRate = 38400;
            break;
        case QSerialPort::Baud57600:
            dataRate = 57600;
            break;
        case QSerialPort::Baud115200:
            dataRate = 115200;
            break;
            // Otherwise do nothing.
        case QSerialPort::UnknownBaud:
        default:
	    //m_port has likely returned an invalid value here. Default to 57600 to make connecting
	    //to a 3DR radio easier.
        if (m_baud != -1)
        {
            dataRate = m_baud;
        }
        else
        {
            dataRate = 57600;
        }
	    if (m_port)
	    {
            m_port->setBaudRate(dataRate);
	    }
            break;
    }
    return dataRate;
}

qint64 SerialLink::getTotalUpstream()
{
    m_statisticsMutex.lock();
    return m_bitsSentTotal / ((MG::TIME::getGroundTimeNow() - m_connectionStartTime) / 1000);
    m_statisticsMutex.unlock();
}

qint64 SerialLink::getCurrentUpstream()
{
    return 0; // TODO
}

qint64 SerialLink::getMaxUpstream()
{
    return 0; // TODO
}

qint64 SerialLink::getBitsSent()
{
    return m_bitsSentTotal;
}

qint64 SerialLink::getBitsReceived()
{
    return m_bitsReceivedTotal;
}

qint64 SerialLink::getTotalDownstream()
{
    m_statisticsMutex.lock();
    return m_bitsReceivedTotal / ((MG::TIME::getGroundTimeNow() - m_connectionStartTime) / 1000);
    m_statisticsMutex.unlock();
}

qint64 SerialLink::getCurrentDownstream()
{
    return 0; // TODO
}

qint64 SerialLink::getMaxDownstream()
{
    return 0; // TODO
}

bool SerialLink::isFullDuplex()
{
    /* Serial connections are always half duplex */
    return false;
}

int SerialLink::getLinkQuality()
{
    /* This feature is not supported with this interface */
    return -1;
}

QString SerialLink::getPortName()
{
    return m_portName;
}

// We should replace the accessors below with one to get the QSerialPort

int SerialLink::getBaudRate()
{
    return getNominalDataRate();
}

int SerialLink::getBaudRateType()
{
    int baudRate;
    if (m_port) {
        baudRate = m_port->baudRate();
    } else {
        baudRate = m_baud;
    }
    return baudRate;
}

int SerialLink::getFlowType()
{
    int flowControl;
    if (m_port) {
        flowControl = m_port->flowControl();
    } else {
        flowControl = m_flowControl;
    }
    return flowControl;
}

int SerialLink::getParityType()
{
    int parity;
    if (m_port) {
        parity = m_port->parity();
    } else {
        parity = m_parity;
    }
    return parity;
}

int SerialLink::getDataBitsType()
{
    int dataBits;
    if (m_port) {
        dataBits = m_port->dataBits();
    } else {
        dataBits = m_dataBits;
    }
    return dataBits;
}

int SerialLink::getStopBitsType()
{
    int stopBits;
    if (m_port) {
        stopBits = m_port->stopBits();
    } else {
        stopBits = m_stopBits;
    }
    return stopBits;
}

int SerialLink::getDataBits()
{
    int ret;
    int dataBits;
    if (m_port) {
        dataBits = m_port->dataBits();
    } else {
        dataBits = m_dataBits;
    }

    switch (dataBits) {
    case QSerialPort::Data5:
        ret = 5;
        break;
    case QSerialPort::Data6:
        ret = 6;
        break;
    case QSerialPort::Data7:
        ret = 7;
        break;
    case QSerialPort::Data8:
        ret = 8;
        break;
    default:
        ret = -1;
        break;
    }
    return ret;
}

int SerialLink::getStopBits()
{
    int stopBits;
    if (m_port) {
        stopBits = m_port->stopBits();
    } else {
        stopBits = m_stopBits;
    }
    int ret = -1;
    switch (stopBits) {
    case QSerialPort::OneStop:
        ret = 1;
        break;
    case QSerialPort::TwoStop:
        ret = 2;
        break;
    default:
        ret = -1;
        break;
    }
    return ret;
}

bool SerialLink::setPortName(QString portName)
{
    QLOG_INFO() << "current portName " << m_portName;
    QLOG_INFO() << "setPortName to " << portName;
    bool accepted = false;
    if ((portName != m_portName)
            && (portName.trimmed().length() > 0)) {
        m_portName = portName.trimmed();
//        m_name = tr("serial port ") + portName.trimmed(); // [TODO] Do we need this?
        if(m_port)
            m_port->setPortName(portName);

        emit nameChanged(m_portName); // [TODO] maybe we can eliminate this
        emit updateLink(this);
        if (m_portBaudMap.contains(m_portName))
        {
            setBaudRate(m_portBaudMap[m_portName]);
        }
        return accepted;
    }
    return false;
}


bool SerialLink::setBaudRateType(int rateIndex)
{
    Q_ASSERT_X(m_port != NULL, "setBaudRateType", "m_port is NULL");
    // These minimum and maximum baud rates were based on those enumerated in qserialport.h
    bool result;
    const int minBaud = (int)QSerialPort::Baud1200;
    const int maxBaud = (int)QSerialPort::Baud115200;

    if (m_port && (rateIndex >= minBaud && rateIndex <= maxBaud))
    {
        result = m_port->setBaudRate(static_cast<QSerialPort::BaudRate>(rateIndex));
        emit updateLink(this);
        return result;
    }

    return false;
}

bool SerialLink::setBaudRateString(const QString& rate)
{
    bool ok;
    int intrate = rate.toInt(&ok);
    if (!ok) return false;
    return setBaudRate(intrate);
}

bool SerialLink::setBaudRate(int rate)
{
    bool accepted = false;
    if (rate != m_baud) {
        m_baud = rate;
        accepted = true;
        m_portBaudMap[m_portName] = rate; //Update baud rate for that port in the map.
        if (m_port)
            accepted = m_port->setBaudRate(rate);
        emit updateLink(this);
    }
    return accepted;
}

bool SerialLink::setFlowType(int flow)
{
    bool accepted = false;
    if (flow != m_flowControl) {
        m_flowControl = static_cast<QSerialPort::FlowControl>(flow);
        accepted = true;
        if (m_port)
            accepted = m_port->setFlowControl(static_cast<QSerialPort::FlowControl>(flow));
        emit updateLink(this);
    }
    return accepted;
}

bool SerialLink::setParityType(int parity)
{
    bool accepted = false;
    if (parity != m_parity) {
        m_parity = static_cast<QSerialPort::Parity>(parity);
        accepted = true;
        if (m_port) {
            switch (parity) {
                case QSerialPort::NoParity:
                accepted = m_port->setParity(QSerialPort::NoParity);
                break;
                case 1: // Odd Parity setting for backwards compatibilty
                    accepted = m_port->setParity(QSerialPort::OddParity);
                    break;
                case QSerialPort::EvenParity:
                    accepted = m_port->setParity(QSerialPort::EvenParity);
                    break;
                case QSerialPort::OddParity:
                    accepted = m_port->setParity(QSerialPort::OddParity);
                    break;
                default:
                    // If none of the above cases matches, there must be an error
                    accepted = false;
                    break;
                }
            emit updateLink(this);
        }
    }
    return accepted;
}


bool SerialLink::setDataBits(int dataBits)
{
    bool accepted = false;
    if (dataBits != m_dataBits) {
        m_dataBits = static_cast<QSerialPort::DataBits>(dataBits);
        accepted = true;
        if (m_port)
            accepted = m_port->setDataBits(static_cast<QSerialPort::DataBits>(dataBits));
        emit updateLink(this);
    }
    return accepted;
}

bool SerialLink::setStopBits(int stopBits)
{
    // Note 3 is OneAndAHalf stopbits.
    bool accepted = false;
    if (stopBits != m_stopBits) {
        m_stopBits = static_cast<QSerialPort::StopBits>(stopBits);
        accepted = true;
        if (m_port)
            accepted = m_port->setStopBits(static_cast<QSerialPort::StopBits>(stopBits));
        emit updateLink(this);
    }
    return accepted;
}

bool SerialLink::setDataBitsType(int dataBits)
{
    bool accepted = false;
    if (dataBits != m_dataBits) {
        m_dataBits = static_cast<QSerialPort::DataBits>(dataBits);
        accepted = true;
        if (m_port)
            accepted = m_port->setDataBits(static_cast<QSerialPort::DataBits>(dataBits));
        emit updateLink(this);
    }
    return accepted;
}

bool SerialLink::setStopBitsType(int stopBits)
{
    bool accepted = false;
    if (stopBits != m_stopBits) {
        m_stopBits = static_cast<QSerialPort::StopBits>(stopBits);
        accepted = true;
        if (m_port)
            accepted = m_port->setStopBits(static_cast<QSerialPort::StopBits>(stopBits));
        emit updateLink(this);
    }
    return accepted;
}

const QList<SerialLink*> SerialLink::getSerialLinks(LinkManager *linkManager)
{
    if(!linkManager)
        return QList<SerialLink*>();

    QList<LinkInterface*> list = linkManager->instance()->getLinks();
    QList<SerialLink*> serialLinklist;
    foreach( LinkInterface* link, list)  {
        SerialLink* serialLink = dynamic_cast<SerialLink*>(link);
        if (serialLink) {
                serialLinklist.append(serialLink);
            }
        };

    return serialLinklist;
}

const QList<SerialLink*> SerialLink::getSerialLinks(UASInterface *uas)
{
    if(!uas)
        return QList<SerialLink*>();

    QList<LinkInterface*>* list = uas->getLinks();
    QList<SerialLink*> serialLinklist;
    foreach( LinkInterface* link, *list)  {
        SerialLink* serialLink = dynamic_cast<SerialLink*>(link);
        if (serialLink) {
                serialLinklist.append(serialLink);
            }
        };

    return serialLinklist;
}
