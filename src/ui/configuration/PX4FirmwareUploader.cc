#include "PX4FirmwareUploader.h"
#include "qserialportinfo.h"
//#include <QtCrypto/qca.h>
#include <QCryptographicHash>
#include <QDateTime>
#include "QsLog.h"
#define PROTO_OK 0x10
#define PROTO_GET_DEVICE 0x22
#define PROTO_EOC 0x20
#define PROTO_DEVICE_BL_REV 0x01
#define PROTO_DEVICE_BOARD_ID 0x02
#define PROTO_DEVICE_BOARD_REV 0x03
#define PROTO_DEVICE_FW_SIZE 0x04
#define PROTO_DEVICE_VEC_AREA 0x05
PX4FirmwareUploader::PX4FirmwareUploader(QObject *parent) :
    QThread(parent)
{
    m_stop = false;
}
QString hmacSha1(QByteArray key, QByteArray baseString)
{
    int blockSize = 64; // HMAC-SHA-1 block size, defined in SHA-1 standard
    if (key.length() > blockSize) { // if key is longer than block size (64), reduce key length with SHA-1 compression
        key = QCryptographicHash::hash(key, QCryptographicHash::Sha1);
    }

    QByteArray innerPadding(blockSize, char(0x36)); // initialize inner padding with char "6"
    QByteArray outerPadding(blockSize, char(0x5c)); // initialize outer padding with char "\"
    // ascii characters 0x36 ("6") and 0x5c ("\") are selected because they have large
    // Hamming distance (http://en.wikipedia.org/wiki/Hamming_distance)

    for (int i = 0; i < key.length(); i++) {
        innerPadding[i] = innerPadding[i] ^ key.at(i); // XOR operation between every byte in key and innerpadding, of key length
        outerPadding[i] = outerPadding[i] ^ key.at(i); // XOR operation between every byte in key and outerpadding, of key length
    }

    // result = hash ( outerPadding CONCAT hash ( innerPadding CONCAT baseString ) ).toBase64
    QByteArray total = outerPadding;
    QByteArray part = innerPadding;
    part.append(baseString);
    total.append(QCryptographicHash::hash(part, QCryptographicHash::Sha1));
    QByteArray hashed = QCryptographicHash::hash(total, QCryptographicHash::Sha1);
    return hashed.toBase64();
}

bool PX4FirmwareUploader::reqInfo(unsigned char infobyte,unsigned int *reply)
{
    m_port->clear();
    //for (int i=0;i<128;i++)
    //{
    //    m_port->write(QByteArray().append((char)0x0));
    //}
    //m_port->waitForBytesWritten(100);
    //m_port->flush();
    //msleep(100);
    //while(m_port->waitForReadyRead(1))
    //{
    //    int num = m_port->read(1)[0];
    //}
    //int sync = get_sync(2000);
    m_port->write(QByteArray().append(PROTO_GET_DEVICE).append(infobyte).append(PROTO_EOC));
    m_port->waitForBytesWritten(-1);
    m_port->flush();
    QByteArray infobuf;
    int read = readBytes(4,5000,infobuf);
    if (read != 4)
    {
        //Bad read
        QLOG_ERROR() << "Tried to read 4, only read:" << read;
    }
    else
    {
        *reply = ((unsigned char)infobuf[0]) + ((unsigned char)infobuf[1] << 8) + ((unsigned char)infobuf[2] << 16) + ((unsigned char)infobuf[3] << 24);
    }
    int sync = get_sync(2000);
    if (sync != 0)
    {
        return false;
    }
    return true;
}
int PX4FirmwareUploader::readBytes(int num,int timeout,QByteArray &buf)
{
    if (m_serialBuffer.size() >= num)
    {
        buf.append(m_serialBuffer.mid(0,num));
        m_serialBuffer.remove(0,num);
        return num;
    }
    int count = -1;
    bool first = true;
    qint64 msec = QDateTime::currentMSecsSinceEpoch();
    //m_serialBuffer
    while (count < num && QDateTime::currentMSecsSinceEpoch() < (msec + timeout))
    {
        while (m_port->waitForReadyRead(timeout))
        {
            m_serialBuffer.append(m_port->readAll());
            if (m_serialBuffer.size() >= num)
            {
                buf.append(m_serialBuffer.mid(0,num));
                m_serialBuffer.remove(0,num);
                return num;
            }
        }
        QLOG_DEBUG() << "timeout expired:" << m_serialBuffer.size() << num;
        return -1;
        if ( m_port->bytesAvailable() > 0 || (m_port->waitForReadyRead(10) && count < num) || first)
        {
            first = false;
            char c;
            while (count < num && m_port->read(&c,1))
            {
                //m_port->waitForReadyRead(1);
                if (count == -1)
                {
                    count = 0;
                }
                count++;
                buf.append(c);
            }
            //QLOG_DEBUG() << "Ready read success" << count << num;
        }
        else
        {
            //QLOG_DEBUG() << "Ready read failure" << count << num;
        }
    }
    return count;
}
void PX4FirmwareUploader::stop()
{
    m_stop = true;
}

bool PX4FirmwareUploader::loadFile(QString file)
{
    QFile json(file);
    json.open(QIODevice::ReadOnly);
    QByteArray jsonbytes = json.readAll();
    QString jsonstring(jsonbytes);
    json.close();
    //This chunk of code is from QUpgrade
    //Available https://github.com/LorenzMeier/qupgrade
    // BOARD ID
    QStringList decode_list = jsonstring.split("\"board_id\":");
    decode_list = decode_list.last().split(",");
    if (decode_list.count() < 1)
    {
        QLOG_ERROR() << "Error parsing BOARD ID from .px4 file";
        return false;
    }
    QString board_id = QString(decode_list.first().toUtf8()).trimmed();
    m_loadedBoardID = board_id.toInt();

    // IMAGE SIZE
    decode_list = jsonstring.split("\"image_size\":");
    decode_list = decode_list.last().split(",");
    if (decode_list.count() < 1)
    {
        QLOG_ERROR() << "Error parsing IMAGE SIZE from .px4 file";
        return false;
    }
    QString image_size = QString(decode_list.first().toUtf8()).trimmed();
    m_loadedFwSize = image_size.toInt();

    // DESCRIPTION
    decode_list = jsonstring.split("\"description\": \"");
    decode_list = decode_list.last().split("\"");
    if (decode_list.count() < 1)
    {
        QLOG_ERROR() << "Error parsing DESCRIPTION from .px4 file";
        return false;
    }
    m_loadedDescription = QString(decode_list.first().toUtf8()).trimmed();
    QStringList list = jsonstring.split("\"image\": \"");
    list = list.last().split("\"");

    QByteArray fwimage;


    fwimage.append((unsigned char)((m_loadedFwSize >> 24) & 0xFF));
    fwimage.append((unsigned char)((m_loadedFwSize >> 16) & 0xFF));
    fwimage.append((unsigned char)((m_loadedFwSize >> 8) & 0xFF));
    fwimage.append((unsigned char)((m_loadedFwSize >> 0) & 0xFF));




    QByteArray raw64 = list.first().toUtf8();

    fwimage.append(QByteArray::fromBase64(raw64));
    QByteArray uncompressed = qUncompress(fwimage);

    QLOG_INFO() << "Firmware size:" << uncompressed.size() << "expected" << m_loadedFwSize << "bytes";
    if (uncompressed.size() != m_loadedFwSize)
    {
        QLOG_ERROR() << "Error in decompressing firmware. Please re-download and try again";
        m_port->close();
        return false;
    }
    //Per QUpgrade, pad it to a 4 byte multiple.
    while ((uncompressed.count() % 4) != 0)
    {
        uncompressed.append(0xFF);
    }
    tempFile = new QTemporaryFile();
    tempFile->open();
    tempFile->write(uncompressed);
    tempFile->close();
    start();
}

void PX4FirmwareUploader::run()
{
    QLOG_INFO() << "Waiting for device to be plugged in...";
    emit requestDevicePlug();
    bool found = false;
    QList<QString> portlist;
    QString portnametouse = "";
    int size = 0;
    foreach (QSerialPortInfo info,QSerialPortInfo::availablePorts())
    {
        portlist.append(info.portName());
    }
    size = portlist.size();
    while (!found)
    {
        foreach (QSerialPortInfo info,QSerialPortInfo::availablePorts())
        {
            if (!portlist.contains(info.portName()))
            {
                portnametouse = info.portName();
                found = true;
                break;
            }
        }
        if (size > QSerialPortInfo::availablePorts().size())
        {
            //Something has been removed. Rescan.
            portlist.clear();
            foreach (QSerialPortInfo info,QSerialPortInfo::availablePorts())
            {
                portlist.append(info.portName());
            }
            size = portlist.size();
        }
        if (m_stop)
        {
            return;
        }
        msleep(100);
    }
    m_port = new QSerialPort();
    msleep(500);
    m_port->setPortName(portnametouse);
    if (!m_port->open(QIODevice::ReadWrite))
    {
        QLOG_ERROR() << "Unable to open port" << m_port->errorString() << m_port->portName();
        return;
    }
    m_port->setBaudRate(QSerialPort::Baud115200);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    m_port->setTextModeEnabled(false);
    for (int i=0;i<128;i++)
    {
        m_port->write(QByteArray().append((char)0x0));
    }
    m_port->waitForBytesWritten(100);
    msleep(1000);
    while(m_port->bytesAvailable())
    {
        int num = m_port->read(1)[0];
    }
    if (m_stop)
    {
        m_port->close();
        delete m_port;
        return;
    }
    for (int retry=0;retry<5;retry++)
    {
        QLOG_INFO() << "Sending SYNC command, loop" << retry << "of" << 5;
        m_port->write(QByteArray().append(0x21).append(0x20));
        m_port->waitForBytesWritten(-1);
        m_port->flush();
        int sync = get_sync();
        if (sync == 0)
        {
            QLOG_INFO() << "Initial Sync successful";
            int bootloaderrev=0;
            int boardid=0;
            int boardrev=0;
            int flashsize=0;
            QString otpstr = "";
            QString snstr = "";
            //we're synced
            unsigned int read = 0;
            emit statusUpdate("Requesting bootloader rev");
            bool reply = reqInfo(PROTO_DEVICE_BL_REV,&read);
            QLOG_INFO() << "Bootloader rev:" << read;
            if (!reply)
            {
                QLOG_WARN() << "Bad sync";
                continue;
            }
            bootloaderrev = read;
            if (bootloaderrev >= 4)
            {
                QLOG_FATAL() << "PX4Firmware Uploader does not yet support V4 bootloaders";
                //return;
            }
            msleep(500);
            emit statusUpdate("Requesting board ID");
            reply = reqInfo(PROTO_DEVICE_BOARD_ID,&read);
            QLOG_INFO() << "Board ID:" << read;
            if (!reply)
            {
                QLOG_WARN() << "Bad sync";
                continue;
            }
            boardid = read;
            msleep(500);
            emit statusUpdate("Requesting board rev");
            reply = reqInfo(PROTO_DEVICE_BOARD_REV,&read);
            QLOG_INFO() << "Board rev:" << read;
            if (!reply)
            {
                QLOG_WARN() << "Bad sync";
                continue;
            }
            boardrev = read;
            msleep(500);
            emit statusUpdate("Requesting firmware size");
            reply = reqInfo(PROTO_DEVICE_FW_SIZE,&read);
            QLOG_INFO() << "Flash size:" << read;
            if (!reply)
            {
                QLOG_WARN() << "Bad sync";
                continue;
            }
            flashsize = read;

            while(m_port->bytesAvailable())
            {
                int num = m_port->read(1)[0];
            }
            if (m_stop)
            {
                m_port->close();
                delete m_port;
                return;
            }

            //Create an empty buffer
            msleep(250);
            unsigned char otpbuf[512];
            memset(otpbuf,0,512);
            bool bad = false;
            int timeout = 0;
            if (bootloaderrev >= 4)
            {
                QLOG_INFO() << "Requesting OTP";
                emit statusUpdate("Requesting OTP");
                for (int i=0;i<512;i+=4)
                {
                    m_port->clear();
                    m_port->write(QByteArray().append(0x2A).append(i & 0xFF).append(((i >> 8) & 0xFF)).append((char)0).append((char)0));//.append(PROTO_EOC));
                    m_port->waitForBytesWritten(-1);
                    m_port->flush();
                    timeout = 0;
                    int bytesread = 0;

                    QByteArray bytes;
                    int count = readBytes(4,2000,bytes);
                    if (count < 4)
                    {
                        QLOG_ERROR() << "wrong bytes available:" << count;
                        //bad = true;
                        i-=4;
                        m_port->waitForReadyRead(1000);
                        while(m_port->bytesAvailable())
                        {
                            int num = m_port->read(1)[0];
                        }
                        //Clear the port
                        m_port->clear();
                        continue;
                    }

                    otpbuf[i] = bytes[0];
                    otpbuf[i+1] = bytes[1];
                    otpbuf[i+2] = bytes[2];
                    otpbuf[i+3] = bytes[3];
                    int sync = get_sync(2000);
                    if (sync != 0)
                    {
                        QLOG_ERROR() << "Bad sync";
                        i-=4;
                        m_port->waitForReadyRead(1000);
                        while(m_port->bytesAvailable())
                        {
                            int num = m_port->read(1)[0];
                        }
                        continue;
                        //bad = true;
                        //break;
                    }
                    if (m_stop)
                    {
                        m_port->close();
                        delete m_port;
                        return;
                    }
                }
                if (bad)
                {
                    continue;
                }
                QLOG_INFO() << "OTP read";
                if (otpbuf[0] != 80 && otpbuf[1] != 88 && otpbuf[2] != 52 && otpbuf[3] != 0)
                {
                    QLOG_ERROR() << "OTP header failure";
                    continue;
                }
                //Let's format this like MP does
                QString otpoutput = "";
                for (int i=0;i<512;i++)
                {
                    otpoutput += (otpbuf[i] <= 0xF ? "0" : "") + QString::number(otpbuf[i],16).toUpper() + " ";
                    if (i % 16 == 15)
                    {
                        QLOG_TRACE() << otpoutput;
                        otpstr += otpoutput + "\n";
                        otpoutput = "";
                    }
                }



                //Create an empty buffer for the serialnumber
                emit statusUpdate("Requesting board SN");
                unsigned char snbuf[12];
                memset(snbuf,0,12);
                for (int i=0;i<12;i+=4)
                {
                    m_port->clear();
                    m_port->write(QByteArray().append(0x2B).append(i).append((char)0).append((char)0).append((char)0).append(PROTO_EOC));
                    m_port->flush();
                    timeout = 0;
                    QByteArray bytes;
                    int count = readBytes(4,2000,bytes);
                    if (count < 4)
                    {
                        QLOG_ERROR() << "wrong bytes available:" << count;
                        //bad = true;
                        i-=4;
                        m_port->waitForReadyRead(1000);
                        while(m_port->bytesAvailable())
                        {
                            int num = m_port->read(1)[0];
                        }
                        continue;
                    }
                    snbuf[i] = bytes[3];
                    snbuf[i+1] = bytes[2];
                    snbuf[i+2] = bytes[1];
                    snbuf[i+3] = bytes[0];
                    int sync = get_sync(2000);
                    if (sync != 0)
                    {
                        QLOG_ERROR() << "Bad sync";
                        bad = true;
                        break;
                    }
                    while(m_port->bytesAvailable())
                    {
                        m_port->read(1)[0];
                    }

                }
                if (bad)
                {
                    continue;
                }
                QString SN = "";
                for (int i=0;i<12;i++)
                {
                    //(otpbuf[i] <= 0xF ? "0" : "") + QString::number(otpbuf[i],16).toUpper() + " ";
                    SN += (snbuf[i] <= 0xF ? "0" : "") + QString::number(snbuf[i],16).toUpper() + " ";
                    //snstr += SN + "\n";
                }

                QLOG_INFO() << "Board SN:" << SN;
                snstr = SN;
                QByteArray signature;
                for (int i=32;i<(128+32);i++)
                {
                    signature.append(otpbuf[i]);
                }
                QByteArray serial;
                for (int i=0;i<12;i++)
                {
                    serial.append(snbuf[i]);
                }
                for (int i=0;i<8;i++)
                {
                    serial.append((char)0);
                }
                emit statusUpdate("Verifying OTP");
                //qDebug() << "Sig size:" << signature.size();
                //qDebug() << "First three of sig:" << QString::number(signature[0],16) << QString::number(signature[1],16) << QString::number(signature[2],16);
                //qDebug() << "Last three of sig:" << QString::number(signature[125],16) << QString::number(signature[126],16) << QString::number(signature[127],16);
                //qDebug() << "Serial size:" << serial.size();

                //msleep(1000);

                /*publicKey.update(serial);
                if (publicKey.validSignature(signature))
                {
                    qDebug() << "Signature is valid!!";
                }
                else
                {
                    qDebug() << "Signature is invalid!";
                }*/
            } //if bootloaderrev >= 4
            if (m_stop)
            {
                m_port->close();
                delete m_port;
                return;
            }


            emit boardRev(boardrev);
            emit boardId(boardid);
            emit bootloaderRev(bootloaderrev);
            emit flashSize(flashsize);
            emit serialNumber(snstr);
            emit OTP(otpstr);

            //m_port->close();
            //return;
            //Erase
            QLOG_INFO() << "Requesting erase";
            emit statusUpdate("Erasing flash, this may take up to a minute");
            m_port->write(QByteArray().append(0x23).append(0x20));
            m_port->flush();
            //msleep(20000);
            sync = get_sync(60000);
            if (sync)
            {
                QLOG_DEBUG() << "never returned from erase.";
                return;
            }
            if (m_stop)
            {
                m_port->close();
                delete m_port;
                return;
            }


            {

                //unsigned int fwsize = fwmap["image_size"].toUInt();
                //Lorenz says that this is a more reliable way of parsing out the image, I agree.
                //Clear out the m_port->
                m_port->clear();
                msleep(1000);

                QLOG_INFO() << "Starting flash process";
                emit statusUpdate("Flashing firmware");
                tempFile->open();
                int counter = 0;
                int failure = 0;
                while (!tempFile->atEnd())
                {
                    QByteArray buf = tempFile->read(60);
                    if (buf.size() > 0)
                    {
                        QByteArray tosend;
                        tosend.append(0x27);
                        tosend.append(buf.size());
                        tosend.append(buf);
                        tosend.append(0x20);
                        m_port->clear();
                        m_port->write(tosend);
                        m_port->waitForBytesWritten(-1);
                        m_port->flush();
                        //msleep(1000);
                        int sync = get_sync(1000);
                        if (sync != 0)
                        {
                            failure++;
                            if (failure > 2)
                            {
                                QLOG_FATAL() << "error writing firmware" << tempFile->pos() << tempFile->size();
                                emit error("Error writing firmware, invalid sync. Please retry");
                                m_port->close();
                                tempFile->close();
                                delete tempFile;
                                return;
                            }
                            msleep(1000);
                            QLOG_INFO() << "Requesting erase";
                            emit statusUpdate("Erasing flash, this may take up to a minute");
                            m_port->clear();
                            m_port->write(QByteArray().append(0x23).append(0x20));
                            m_port->flush();
                            //msleep(20000);
                            sync = get_sync(60000);
                            if (sync)
                            {
                                QLOG_DEBUG() << "never returned from erase.";
                                return;
                            }
                            tempFile->seek(0);
                            continue;
                        }
                        if (counter++ % 50 == 0)
                        {
                            emit flashProgress(tempFile->pos(),tempFile->size());
                            QLOG_INFO() << "flashing:" << tempFile->pos() << "/" << tempFile->size();
                        }
                        if (m_stop)
                        {
                            m_port->close();
                            tempFile->close();
                            delete tempFile;
                            delete m_port;
                            return;
                        }

                    }
                    else
                    {
                        QLOG_ERROR() << "Something went wrong, couldn't read from tmp file";
                        m_port->close();
                        tempFile->close();
                        delete tempFile;
                        delete m_port;
                        return;
                    }
                }
                QLOG_DEBUG() << "Done";
                emit statusUpdate("Flashing complete!");
                m_port->write(QByteArray().append(0x30).append(0x20));
                m_port->flush();
                m_port->waitForBytesWritten(1000);
                m_port->close();
                tempFile->close();
                delete tempFile;
                delete m_port;
                emit done();
                return;


            }


            m_port->close();
        }
        else
        {
            //qDebug() << "Attempting to reboot";
            continue;
            //Reboot

            // Send command to reboot via NSH, then via MAVLink
            // XXX hacky but safe
            // Start NSH
            const char init[] = {0x0d, 0x0d, 0x0d};
            m_port->write(init, sizeof(init));
            m_port->flush();

            // Reboot into bootloader
            const char* cmd = "reboot -b\n";
            m_port->write(cmd, strlen(cmd));
            m_port->flush();
            m_port->write(init, 2);
            m_port->flush();

            // Old reboot command
            const char* cmd_old = "reboot\n";
            m_port->write(cmd_old, strlen(cmd_old));
            m_port->flush();
            m_port->write(init, 2);
            m_port->flush();

            // Reboot via MAVLink (if enabled)
            // Try system ID 1
            const char mavlink_msg_id1[] = {0xfe,0x21,0x72,0xff,0x00,0x4c,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf6,0x00,0x01,0x00,0x00,0x48,0xf0};
            m_port->write(mavlink_msg_id1, sizeof(mavlink_msg_id1));
            m_port->flush();
            // Try system ID 0 (broadcast)
            const char mavlink_msg_id0[] = {0xfe,0x21,0x45,0xff,0x00,0x4c,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf6,0x00,0x00,0x00,0x00,0xd7,0xac};
            m_port->write(mavlink_msg_id0, sizeof(mavlink_msg_id0));
            m_port->flush();
            //m_port->close();
            msleep(1000);
            if (!m_port->open(QIODevice::ReadWrite))
            {
                //return;
            }
            m_port->waitForReadyRead(1000);
            m_port->write(QByteArray().append(0x21).append(0x20));
            int sync = get_sync();
        }
    }

}
int PX4FirmwareUploader::get_sync(int timeout)
{
    QByteArray infobuf;
    int read = readBytes(2,timeout,infobuf);
    if (read != 2)
    {
        QLOG_ERROR() << "Wrong number of bytes read on sync:" << read;
        return -1;
    }
    else
    {
        if (infobuf[0] != (char)0x12  || infobuf[1] != (char)0x10)
        {
            QLOG_ERROR() << "Bad sync return:" << QString::number(infobuf[0],16) << QString::number(infobuf[1],16);
            return -1;
        }
        return 0;
    }
}
