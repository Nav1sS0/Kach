#include "Server.h"
#include <QDebug>
#include <QDataStream>
#include <QBuffer>
#include <QDate>
#include <QDateTime>

Server::Server(QObject *parent)
    : QTcpServer(parent),
    requireSize(0),
    comlexData(false),
    expectedImageSize(0),
    currentImageUserId(-1),
    isReceivingImage(false),
    currentMeasurementId(-1),
    inactivityCheckTimer(nullptr)
{
    if (this->listen(QHostAddress::Any, 2323)) {
        qDebug() << "=== Server started on port 2323 ===";
    } else {
        qDebug() << "=== Server error: " << this->errorString() << " ===";
    }

    // ✅ Запускаем таймер проверки неактивных соединений
    inactivityCheckTimer = new QTimer(this);
    connect(inactivityCheckTimer, &QTimer::timeout, this, &Server::checkInactiveConnections);
    inactivityCheckTimer->start(5000);  // Проверяем каждые 5 секунд
    qDebug() << "DDoS Protection: Inactivity checker started";
}

Server::~Server()
{
    if (inactivityCheckTimer) {
        inactivityCheckTimer->stop();
        inactivityCheckTimer->deleteLater();
    }

    for(auto& socketInfo : Sockets) {
        if(socketInfo.socket) {
            socketInfo.socket->disconnectFromHost();
            delete socketInfo.socket;
        }
    }
    Sockets.clear();
    ipConnectionCount.clear();
    ipPacketTimestamps.clear();
}

void Server::incomingConnection(qintptr socketDescriptor)
{
    // Создаём сокет сразу — безопасно получаем IP без tempSocket
    QTcpSocket* socket = new QTcpSocket;
    if (!socket->setSocketDescriptor(socketDescriptor)) {
        qWarning() << "Failed to set socket descriptor";
        delete socket;
        return;
    }

    QString clientIp = socket->peerAddress().toString();
    qDebug() << "Incoming connection from:" << clientIp;

    // ПРОВЕРКА 1: Максимум соединений в целом
    if (Sockets.size() >= MAX_TOTAL_CONNECTIONS) {
        qWarning() << "DDoS Protection: Max total connections reached ("
                   << MAX_TOTAL_CONNECTIONS << "). Rejecting:" << clientIp;
        socket->close();
        delete socket;
        return;
    }

    // ПРОВЕРКА 2: Максимум соединений с одного IP
    if (ipConnectionCount[clientIp] >= MAX_CONNECTIONS_PER_IP) {
        qWarning() << "DDoS Protection: Too many connections from IP:" << clientIp
                   << "(" << ipConnectionCount[clientIp] << "/" << MAX_CONNECTIONS_PER_IP << ")";
        socket->close();
        delete socket;
        return;
    }

    // Устанавливаем таймауты на сокет
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(socket, &QTcpSocket::readyRead, this, &Server::slotReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &Server::removeSocket);

    // Увеличиваем счетчик для этого IP
    ipConnectionCount[clientIp]++;

    SocketInfo si;
    si.socket = socket;
    si.authenticated = false;
    si.expectedSize = 0;
    si.clientIp = clientIp;
    si.connectionTime = QDateTime::currentDateTime();
    si.lastActivityTime = QDateTime::currentDateTime();
    si.bytesReceived = 0;
    si.packetCount = 0;
    Sockets.append(si);

    qDebug() << "Client connected. IP:" << clientIp
             << "| Total clients:" << Sockets.size()
             << "| From this IP:" << ipConnectionCount[clientIp];
}

void Server::removeSocket()
{
    for(int i = Sockets.size() - 1; i >= 0; i--) {
        if(Sockets[i].socket == sender()) {
            QString clientIp = Sockets[i].clientIp;

            // Уменьшаем счетчик для этого IP
            ipConnectionCount[clientIp]--;
            if (ipConnectionCount[clientIp] <= 0) {
                ipConnectionCount.remove(clientIp);
                ipPacketTimestamps.remove(clientIp);
            }

            Sockets.removeAt(i);
            break;
        }
    }

    QTcpSocket* socket = dynamic_cast<QTcpSocket*>(sender());
    if(socket) {
        delete socket;
    }

    qDebug() << "Client disconnected. Total clients:" << Sockets.size();
}

bool Server::isRateLimited(const QString& clientIp)
{
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64>& timestamps = ipPacketTimestamps[clientIp];

    // Удаляем пакеты старше 1 секунды
    while (!timestamps.isEmpty() && timestamps.first() < currentTime - 1000) {
        timestamps.removeFirst();
    }

    // Проверяем лимит пакетов в секунду
    return timestamps.size() > MAX_PACKETS_PER_SECOND;
}

void Server::checkInactiveConnections()
{
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    QList<int> indicesToRemove;

    for (int i = 0; i < Sockets.size(); ++i) {
        SocketInfo& si = Sockets[i];

        if (!si.socket || !si.socket->isOpen()) {
            indicesToRemove.append(i);
            continue;
        }

        qint64 inactiveMs = currentTime - si.lastActivityTime.toMSecsSinceEpoch();
        qint64 connectedMs = currentTime - si.connectionTime.toMSecsSinceEpoch();

        // Таймаут подключения без аутентификации
        if (!si.authenticated && connectedMs > CONNECTION_TIMEOUT_MS) {
            qWarning() << "DDoS Protection: Unauthenticated connection timeout from:" << si.clientIp;
            indicesToRemove.append(i);
            continue;
        }

        // Для аутентифицированного: таймаут неактивности
        if (si.authenticated && inactiveMs > INACTIVITY_TIMEOUT_MS) {
            qWarning() << "DDoS Protection: Inactivity timeout from:" << si.clientIp;
            indicesToRemove.append(i);
            continue;
        }
    }

    // Закрываем неактивные соединения в обратном порядке
    for (int i = indicesToRemove.size() - 1; i >= 0; --i) {
        int idx = indicesToRemove[i];
        if (idx >= 0 && idx < Sockets.size()) {
            if (Sockets[idx].socket) {
                Sockets[idx].socket->close();
            }
        }
    }
}

void Server::SendToClient(QTcpSocket* clientSocket, const QString& str)
{
    if (!clientSocket || !clientSocket->isOpen()) {
        qDebug() << "ERROR: Cannot send to client - socket is not open";
        return;
    }

    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_DefaultCompiledVersion);
    out << str;

    qint64 written = clientSocket->write(data);
    clientSocket->flush();

    qDebug() << "SendToClient:" << str.left(80) << "| Bytes:" << written;
}

void Server::SendImageToClient(QTcpSocket* clientSocket, int user_id, const QByteArray& imageData)
{
    if (!clientSocket || !clientSocket->isOpen()) {
        qDebug() << "ERROR: Cannot send image to client - socket is not open";
        return;
    }

    qDebug() << "=== SendImageToClient (Avatar) ===";
    qDebug() << "User ID:" << user_id << "Image size:" << imageData.size();

    QString description = QString("LOADIMGAVATAR|%1|%2").arg(user_id).arg(imageData.size());

    QByteArray fullData;
    QDataStream out(&fullData, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_DefaultCompiledVersion);

    out << description;
    out.writeRawData(imageData.constData(), imageData.size());

    qDebug() << "Total packet size:" << fullData.size();

    qint64 written = clientSocket->write(fullData);
    clientSocket->flush();

    qDebug() << "Image sent. Bytes written:" << written;
}

void Server::SendImageToClient(QTcpSocket* clientSocket, int measurement_id, const QByteArray& imageData, const QString& photoType)
{
    if (!clientSocket || !clientSocket->isOpen()) {
        qDebug() << "ERROR: Cannot send image to client - socket is not open";
        return;
    }

    qDebug() << "=== SendImageToClient (Measurement Photo) ===";
    qDebug() << "Measurement ID:" << measurement_id << "Type:" << photoType << "Image size:" << imageData.size();

    QString description = QString("LOADMEASUREMENTPHOTO|%1|%2|%3").arg(measurement_id).arg(photoType).arg(imageData.size());

    QByteArray fullData;
    QDataStream out(&fullData, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_DefaultCompiledVersion);

    out << description;
    out.writeRawData(imageData.constData(), imageData.size());

    qDebug() << "Total packet size:" << fullData.size();

    qint64 written = clientSocket->write(fullData);
    clientSocket->flush();

    qDebug() << "Photo sent. Bytes written:" << written;
}

void Server::slotReadyRead()
{
    QTcpSocket* currentSocket = static_cast<QTcpSocket*>(sender());

    if (!currentSocket || !currentSocket->isOpen()) {
        qDebug() << "ERROR: Socket is not valid or not open";
        return;
    }

    int index = findSocketIndex(currentSocket);
    if(index == -1) {
        qDebug() << "ERROR: Socket not found in list";
        return;
    }

    SocketInfo& si = Sockets[index];

    // Обновляем время последней активности
    si.lastActivityTime = QDateTime::currentDateTime();
    si.packetCount++;

    // Читаем все доступные данные один раз
    QByteArray newData = currentSocket->readAll();
    si.bytesReceived += newData.size();

    // Если мы в процессе получения изображения
    if(isReceivingImage && expectedImageSize > 0) {
        qDebug() << "Receiving image data... Current:" << imageBuffer.size() << "Expected:" << expectedImageSize;

        if(!newData.isEmpty()) {
            imageBuffer.append(newData);
            qDebug() << "Appended" << newData.size() << "bytes. Total:" << imageBuffer.size();
        }

        if(imageBuffer.size() >= (qint64)expectedImageSize) {
            qDebug() << "Image complete! Processing...";

            QByteArray completeImage = imageBuffer.left(expectedImageSize);
            imageBuffer.clear();

            if(currentMeasurementId > 0) {
                // Фото замера — отвечаем PHOTO_SAVED
                qDebug() << "=== saveMeasurementPhoto === User:" << currentImageUserId
                         << "Measurement:" << currentMeasurementId << "Type:" << currentPhotoType;
                if(UserManagers.saveMeasurementPhoto(currentImageUserId, currentMeasurementId, currentPhotoType, completeImage)) {
                    SendToClient(currentSocket, QString("PHOTO_SAVED|%1|%2").arg(currentMeasurementId).arg(currentPhotoType));
                    qDebug() << "Photo saved successfully for measurement" << currentMeasurementId;
                } else {
                    SendToClient(currentSocket, "PHOTO_SAVE_FAILED");
                    qDebug() << "Error saving measurement photo";
                }
            } else {
                // Аватар — отвечаем IMAGE_SAVED_SUCCESSFULLY
                if(UserManagers.saveImage(currentImageUserId, completeImage)) {
                    SendToClient(currentSocket, "IMAGE_SAVED_SUCCESSFULLY");
                    qDebug() << "Avatar saved for user" << currentImageUserId;
                } else {
                    SendToClient(currentSocket, "DATABASE_SAVE_ERROR");
                    qDebug() << "Error saving avatar to database";
                }
            }

            isReceivingImage = false;
            expectedImageSize = 0;
            currentImageUserId = -1;
            currentMeasurementId = -1;
            currentPhotoType = "";
        }
        return;
    }

    // Обработка команд — буферизация для больших пакетов
    si.buffer.append(newData);

    while (si.buffer.size() > 0) {
        // Если ещё не знаем размер пакета, читаем первые 4 байта
        if (si.expectedSize == 0) {
            if (si.buffer.size() < (int)sizeof(quint32))
                break; // ждём ещё данных

            QDataStream sizeStream(si.buffer.left(sizeof(quint32)));
            sizeStream.setVersion(QDataStream::Qt_DefaultCompiledVersion);

            // QString в QDataStream: сначала quint32 длина в байтах, потом данные UTF-16
            quint32 strByteLen;
            sizeStream >> strByteLen;

            if (strByteLen == 0xFFFFFFFF) {
                // null-строка
                si.expectedSize = 0;
                si.buffer.remove(0, sizeof(quint32));
                continue;
            }

            // Полный размер пакета: 4 байта (длина строки) + strByteLen (данные UTF-16)
            si.expectedSize = sizeof(quint32) + strByteLen;
        }

        // Проверяем, что весь пакет уже в буфере
        if ((quint32)si.buffer.size() < si.expectedSize)
            break; // ждём ещё данных

        // Извлекаем полный пакет
        QByteArray packet = si.buffer.left(si.expectedSize);
        si.buffer.remove(0, si.expectedSize);
        si.expectedSize = 0;

        QDataStream in(packet);
        in.setVersion(QDataStream::Qt_DefaultCompiledVersion);

        QString command;
        in >> command;

        if (command.isEmpty())
            continue;

        qDebug() << "=== Command received ===" << command.left(100);

        processCommand(currentSocket, command);
    }
}

void Server::processCommand(QTcpSocket* currentSocket, const QString& command)
{
    int index = findSocketIndex(currentSocket);
    if(index == -1) return;

    SocketInfo& info = Sockets[index];

    //AUTH команда
    if(command.startsWith("AUTH|")) {
        qDebug() << "🔑 AUTH command received from:" << Sockets[index].clientIp;  // ✅ Отладка
        QStringList parts = command.split("|");
        if(parts.size() >= 3) {
            QString login = parts.at(1);
            QString password = parts.at(2);

            int userId;
            if(UserManagers.authenticateUser(login, password, userId)) {
                info.authenticated = true;
                info.userId = userId;
                SendToClient(currentSocket, QString("AUTHORIZED|%1").arg(userId));
                qDebug() << "User authorized:" << login << "ID:" << userId;
            } else {
                SendToClient(currentSocket, "AUTHORIZATION_FAILED");
                qDebug() << "Authorization failed for:" << login;
            }
        }
    }
    //REG команда
    else if(command.startsWith("REG|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 9) {
            QString first_name = parts.at(1);
            QString last_name = parts.at(2);
            QString email = parts.at(3);
            QString password = parts.at(4);
            QString gender = parts.at(5);
            QString birthday = parts.at(6);
            QString height = parts.at(7);
            QString weight = parts.at(8);

            QString errorMessage;
            int userId;
            if(UserManagers.registerUser(first_name, last_name, email, password,
                                          gender, birthday, height.toDouble(),
                                          weight.toDouble(), errorMessage, userId)) {
                info.authenticated = true;
                info.userId = userId;
                SendToClient(currentSocket, QString("REGISTERED|%1").arg(userId));
                qDebug() << "User registered:" << email << "ID:" << userId;
            } else {
                SendToClient(currentSocket, "REGISTRATION_FAILED|" + errorMessage);
                qDebug() << "Registration failed:" << errorMessage;
            }
        }
    }
    //LoadUserData команда
    else if(command.startsWith("LoadUserData|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();

            // Сохраняем userId в сокете — нужно для push-уведомлений (запросы в друзья и т.п.)
            // На Android сессия восстанавливается без AUTH, поэтому userId ставим здесь.
            if (info.userId == -1)
                info.userId = user_id;

            QString first_name;
            QString last_name;
            QString gender;
            QString age;
            double height = 0;
            double weight = 0;
            QString goal = "...";
            int planTrainningUserData = 0;
            int standardSubscription = 0;

            if(UserManagers.loadUserData(user_id, first_name, last_name, gender,
                                          age, height, weight, goal, planTrainningUserData, standardSubscription)) {
                SendToClient(currentSocket,
                             QString("LOADUSERDATA|%1|%2|%3|%4|%5|%6|%7|%8|%9")
                                 .arg(first_name).arg(last_name).arg(gender).arg(age)
                                 .arg((int)height).arg((int)weight).arg(goal)
                                 .arg(planTrainningUserData).arg(standardSubscription));

                qDebug() << "User data loaded for user ID:" << user_id << "subscription:" << standardSubscription;
            } else {
                SendToClient(currentSocket, "LOADUSERDATA_FAILED");
                qDebug() << "Failed to load user data for ID:" << user_id;
            }
        }
    }
    //UPDATE команда
    else if(command.startsWith("UPDATE|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 9) {
            int user_id = parts.at(1).toInt();
            QString first_name = parts.at(2);
            QString last_name = parts.at(3);
            QString gender = parts.at(4);
            QString age = parts.at(5);
            double height = parts.at(6).toDouble();
            double weight = parts.at(7).toDouble();
            QString goal = parts.at(8);

            if(UserManagers.updateUserData(user_id, first_name, last_name, gender,
                                            age, height, weight, goal)) {
                SendToClient(currentSocket, "UPDATEUSERDATA_SUCCESSFULLY");
                qDebug() << "User data updated for ID:" << user_id;
            } else {
                SendToClient(currentSocket, "UPDATEUSERDATA_FAILED");
                qDebug() << "Failed to update user data for ID:" << user_id;
            }
        }
    }
    //IMGAVATAR команда (загрузка изображения на сервер)
    else if(command.startsWith("IMGAVATAR|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            bool ok;
            quint64 imgSize = parts.at(2).toULongLong(&ok);
            quint64 maxImageSize = 1024 * 1024 * 100; // 100 MB

            qDebug() << "IMGAVATAR request: user_id=" << user_id << "size=" << imgSize;

            if(ok && imgSize > 0 && imgSize <= maxImageSize) {
                // Читаем оставшиеся данные после заголовка команды
                QByteArray remainingData = currentSocket->readAll();

                qDebug() << "Remaining bytes:" << remainingData.size() << "Need:" << imgSize;

                if(remainingData.size() >= (qint64)imgSize) {
                    // Все данные получены в одном пакете
                    QByteArray imageData = remainingData.left(imgSize);

                    if(UserManagers.saveImage(user_id, imageData)) {
                        SendToClient(currentSocket, "IMAGE_SAVED_SUCCESSFULLY");
                        qDebug() << "Image saved for user" << user_id;
                    } else {
                        SendToClient(currentSocket, "DATABASE_SAVE_ERROR");
                        qDebug() << "Error saving image";
                    }
                } else {
                    // Данные придут несколькими пакетами
                    imageBuffer.clear();
                    imageBuffer.append(remainingData);
                    expectedImageSize = imgSize;
                    currentImageUserId = user_id;
                    currentMeasurementId = -1;   // это аватар, не замер
                    currentPhotoType = "";
                    isReceivingImage = true;

                    qDebug() << "Image receiving started. Have:" << imageBuffer.size()
                             << "Need:" << expectedImageSize;
                }
            } else {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                qDebug() << "Invalid image size or format";
            }
        }
    }
    //LOADIMGAVATAR команда (загрузка изображения с сервера)
    else if(command.startsWith("LOADIMGAVATAR|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();

            qDebug() << "LOADIMGAVATAR request for user_id:" << user_id;

            QByteArray imageData;
            if(UserManagers.getImage(user_id, imageData)) {
                if(!imageData.isEmpty()) {
                    SendImageToClient(currentSocket, user_id, imageData);
                    qDebug() << "Image sent for user" << user_id;
                } else {
                    SendToClient(currentSocket, "NO_AVATAR_FOUND");
                    qDebug() << "No avatar found for user" << user_id;
                }
            } else {
                SendToClient(currentSocket, "DATABASE_LOAD_ERROR");
                qDebug() << "Error loading image from database";
            }
        }
    }
    //PLAN_TRAINNING_USER_DATA команда
    else if(command.startsWith("PLAN_TRAINNING_USER_DATA|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            int planTrainningUser = parts.at(2).toInt();

            if(UserManagers.saveEPlanTrainningUserData(user_id, planTrainningUser)) {
                SendToClient(currentSocket, "PLAN_TRAINNING_SAVED_SUCCESSFULLY");
                qDebug() << "Plan training data saved for user" << user_id;
            } else {
                SendToClient(currentSocket, "DATABASE_SAVE_ERROR");
                qDebug() << "Error saving plan training data";
            }
        }
    }
    //CREATEMEASUREMENT команда
    else if(command.startsWith("CREATEMEASUREMENT|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 8) {
            int user_id = parts.at(1).toInt();
            QString date = parts.at(2);
            QString weight = parts.at(3);
            QString neck = parts.at(4);
            QString waist = parts.at(5);
            QString hips = parts.at(6);
            QString steps = parts.at(7);

            int measurement_id = -1;
            if(UserManagers.createMeasurement(user_id, date, weight, neck, waist, hips, steps, measurement_id)) {
                SendToClient(currentSocket, QString("CREATEMEASUREMENT_SUCCESS|%1").arg(measurement_id));
                qDebug() << "Measurement created for user" << user_id << "ID:" << measurement_id;
            } else {
                SendToClient(currentSocket, "CREATEMEASUREMENT_FAILED");
                qDebug() << "Failed to create measurement";
            }
        }
    }
    // LOADMEASUREMENTPHOTO команда
    else if(command.startsWith("LOADMEASUREMENTPHOTO|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 3) {
            int measurement_id = parts.at(1).toInt();
            QString photoType = parts.at(2);

            qDebug() << "LOADMEASUREMENTPHOTO request: measurement_id=" << measurement_id << "type=" << photoType;

            QByteArray photoData;
            if(UserManagers.getMeasurementPhoto(measurement_id, photoType, photoData)) {
                if(!photoData.isEmpty()) {
                    SendImageToClient(currentSocket, measurement_id, photoData, photoType);
                    qDebug() << "Photo sent for measurement" << measurement_id << "type:" << photoType;
                } else {
                    SendToClient(currentSocket, "NO_PHOTO_FOUND");
                    qDebug() << "No photo found for measurement" << measurement_id;
                }
            } else {
                SendToClient(currentSocket, "DATABASE_LOAD_ERROR");
                qDebug() << "Error loading photo";
            }
        }
    }
    //UPLOAD_MEASUREMENT_PHOTO команда
    else if(command.startsWith("UPLOAD_MEASUREMENT_PHOTO|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            int measurement_id = parts.at(2).toInt();
            QString photoType = parts.at(3);
            bool ok;
            quint64 imgSize = parts.at(4).toULongLong(&ok);
            quint64 maxImageSize = 1024 * 1024 * 100;

            qDebug() << "UPLOAD_MEASUREMENT_PHOTO: user_id=" << user_id << "measurement_id=" << measurement_id
                     << "type=" << photoType << "size=" << imgSize;

            if(ok && imgSize > 0 && imgSize <= maxImageSize) {
                QByteArray remainingData = currentSocket->readAll();
                qDebug() << "Remaining bytes:" << remainingData.size() << "Need:" << imgSize;

                if(remainingData.size() >= (qint64)imgSize) {
                    qDebug() << "Photo complete in single packet";
                    QByteArray imageData = remainingData.left(imgSize);

                    if(UserManagers.saveMeasurementPhoto(user_id, measurement_id, photoType, imageData)) {
                        SendToClient(currentSocket, QString("PHOTO_SAVED|%1|%2").arg(measurement_id).arg(photoType));
                        qDebug() << "Photo saved successfully";
                    } else {
                        SendToClient(currentSocket, "PHOTO_SAVE_FAILED");
                        qDebug() << "Error saving photo";
                    }
                } else {
                    qDebug() << "Photo will come in parts";
                    imageBuffer.clear();
                    imageBuffer.append(remainingData);
                    expectedImageSize = imgSize;
                    currentImageUserId = user_id;
                    currentMeasurementId = measurement_id;
                    currentPhotoType = photoType;
                    isReceivingImage = true;

                    qDebug() << "Waiting for rest of image. Have:" << imageBuffer.size()
                             << "Need:" << expectedImageSize;
                }
            } else {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                qDebug() << "Invalid image size or format";
            }
        }
    }
    //LOADMEASUREMENTS команда
    else if(command.startsWith("LOADMEASUREMENTS|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();

            qDebug() << "LOADMEASUREMENTS request for user_id:" << user_id;

            QString jsonData;
            if(UserManagers.loadUserMeasurements(user_id, jsonData)) {
                SendToClient(currentSocket, QString("LOADMEASUREMENTS|%1").arg(jsonData));
                qDebug() << "Measurements loaded for user" << user_id;
            } else {
                SendToClient(currentSocket, "LOADMEASUREMENTS_FAILED");
                qDebug() << "Failed to load measurements for user" << user_id;
            }
        }
    }

    // ✅ ==================== ТРЕНИРОВКИ (19 УПРАЖНЕНИЙ) ====================
    // ✅ 1. VERTICAL_THRUST_P1
    else if(command.startsWith("SAVE_TRAINING_VERTICAL_THRUST_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingVerticalThrustP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_VERTICAL_THRUST_P1_SUCCESS");
                qDebug() << "✅ SAVED: VERTICAL_THRUST_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_VERTICAL_THRUST_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_VERTICAL_THRUST_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingVerticalThrustP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_VERTICAL_THRUST_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: VERTICAL_THRUST_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_VERTICAL_THRUST_P1_FAILED");
            }
        }
    }

    // ✅ 2. BUTTOCK_BRIDGE_P1
    else if(command.startsWith("SAVE_TRAINING_BUTTOCK_BRIDGE_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingButtockBridgeP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_BUTTOCK_BRIDGE_P1_SUCCESS");
                qDebug() << "✅ SAVED: BUTTOCK_BRIDGE_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_BUTTOCK_BRIDGE_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_BUTTOCK_BRIDGE_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingButtockBridgeP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_BUTTOCK_BRIDGE_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: BUTTOCK_BRIDGE_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_BUTTOCK_BRIDGE_P1_FAILED");
            }
        }
    }

    // ✅ 3. CHEST_PRESS_P1
    else if(command.startsWith("SAVE_TRAINING_CHEST_PRESS_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingChestPressP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_CHEST_PRESS_P1_SUCCESS");
                qDebug() << "✅ SAVED: CHEST_PRESS_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_CHEST_PRESS_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CHEST_PRESS_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingChestPressP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_CHEST_PRESS_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: CHEST_PRESS_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_CHEST_PRESS_P1_FAILED");
            }
        }
    }

    // ✅ 4. ONE_LEG_BENCH_PRESS_P1
    else if(command.startsWith("SAVE_TRAINING_ONE_LEG_BENCH_PRESS_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingOneLegBenchPressP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_ONE_LEG_BENCH_PRESS_P1_SUCCESS");
                qDebug() << "✅ SAVED: ONE_LEG_BENCH_PRESS_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_ONE_LEG_BENCH_PRESS_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_ONE_LEG_BENCH_PRESS_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingOneLegBenchPressP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_ONE_LEG_BENCH_PRESS_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: ONE_LEG_BENCH_PRESS_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_ONE_LEG_BENCH_PRESS_P1_FAILED");
            }
        }
    }

    // ✅ 5. HORIZONTAL_THRUST_P1
    else if(command.startsWith("SAVE_TRAINING_HORIZONTAL_THRUST_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingHorizontalThrustP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_HORIZONTAL_THRUST_P1_SUCCESS");
                qDebug() << "✅ SAVED: HORIZONTAL_THRUST_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_HORIZONTAL_THRUST_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_HORIZONTAL_THRUST_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingHorizontalThrustP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_HORIZONTAL_THRUST_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: HORIZONTAL_THRUST_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_HORIZONTAL_THRUST_P1_FAILED");
            }
        }
    }

    // ✅ 6. PRESS_ON_MAT_P1
    else if(command.startsWith("SAVE_TRAINING_PRESS_ON_MAT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingPressOnMatP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_PRESS_ON_MAT_P1_SUCCESS");
                qDebug() << "✅ SAVED: PRESS_ON_MAT_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_PRESS_ON_MAT_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_PRESS_ON_MAT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingPressOnMatP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_PRESS_ON_MAT_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: PRESS_ON_MAT_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_PRESS_ON_MAT_P1_FAILED");
            }
        }
    }

    // ✅ 7. PRESS_ON_BALL_P1
    else if(command.startsWith("SAVE_TRAINING_PRESS_ON_BALL_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingPressOnBallP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_PRESS_ON_BALL_P1_SUCCESS");
                qDebug() << "✅ SAVED: PRESS_ON_BALL_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_PRESS_ON_BALL_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_PRESS_ON_BALL_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingPressOnBallP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_PRESS_ON_BALL_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: PRESS_ON_BALL_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_PRESS_ON_BALL_P1_FAILED");
            }
        }
    }

    // ✅ 8. KETTLEBELL_SQUAT_P1
    else if(command.startsWith("SAVE_TRAINING_KETTLEBELL_SQUAT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingKettlebellSquatP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_KETTLEBELL_SQUAT_P1_SUCCESS");
                qDebug() << "✅ SAVED: KETTLEBELL_SQUAT_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_KETTLEBELL_SQUAT_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_KETTLEBELL_SQUAT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingKettlebellSquatP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_KETTLEBELL_SQUAT_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: KETTLEBELL_SQUAT_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_KETTLEBELL_SQUAT_P1_FAILED");
            }
        }
    }

    // ✅ 9. ROMANIAN_DEADLIFT_P1
    else if(command.startsWith("SAVE_TRAINING_ROMANIAN_DEADLIFT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingRomanianDeadliftP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_ROMANIAN_DEADLIFT_P1_SUCCESS");
                qDebug() << "✅ SAVED: ROMANIAN_DEADLIFT_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_ROMANIAN_DEADLIFT_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_ROMANIAN_DEADLIFT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingRomanianDeadliftP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_ROMANIAN_DEADLIFT_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: ROMANIAN_DEADLIFT_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_ROMANIAN_DEADLIFT_P1_FAILED");
            }
        }
    }

    // ✅ 10. KNEE_PUSHUP_P1
    else if(command.startsWith("SAVE_TRAINING_KNEE_PUSHUP_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingKneePushupP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_KNEE_PUSHUP_P1_SUCCESS");
                qDebug() << "✅ SAVED: KNEE_PUSHUP_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_KNEE_PUSHUP_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_KNEE_PUSHUP_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingKneePushupP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_KNEE_PUSHUP_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: KNEE_PUSHUP_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_KNEE_PUSHUP_P1_FAILED");
            }
        }
    }

    // ✅ 11. HIP_ABDUCTION_P1
    else if(command.startsWith("SAVE_TRAINING_HIP_ABDUCTION_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingHipAbductionP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_HIP_ABDUCTION_P1_SUCCESS");
                qDebug() << "✅ SAVED: HIP_ABDUCTION_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_HIP_ABDUCTION_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_HIP_ABDUCTION_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingHipAbductionP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_HIP_ABDUCTION_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: HIP_ABDUCTION_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_HIP_ABDUCTION_P1_FAILED");
            }
        }
    }

    // ✅ 12. TRICEP_EXTENSION_P1
    else if(command.startsWith("SAVE_TRAINING_TRICEP_EXTENSION_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingTricepExtensionP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_TRICEP_EXTENSION_P1_SUCCESS");
                qDebug() << "✅ SAVED: TRICEP_EXTENSION_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_TRICEP_EXTENSION_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_TRICEP_EXTENSION_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingTricepExtensionP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_TRICEP_EXTENSION_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: TRICEP_EXTENSION_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_TRICEP_EXTENSION_P1_FAILED");
            }
        }
    }

    // ✅ 13. ASSISTED_PULLUP_P1
    else if(command.startsWith("SAVE_TRAINING_ASSISTED_PULLUP_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingAssistedPullupP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_ASSISTED_PULLUP_P1_SUCCESS");
                qDebug() << "✅ SAVED: ASSISTED_PULLUP_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_ASSISTED_PULLUP_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_ASSISTED_PULLUP_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingAssistedPullupP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_ASSISTED_PULLUP_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: ASSISTED_PULLUP_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_ASSISTED_PULLUP_P1_FAILED");
            }
        }
    }

    // ✅ 14. BULGARIAN_SPLIT_SQUAT_P1
    else if(command.startsWith("SAVE_TRAINING_BULGARIAN_SPLIT_SQUAT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingBulgarianSplitSquatP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_BULGARIAN_SPLIT_SQUAT_P1_SUCCESS");
                qDebug() << "✅ SAVED: BULGARIAN_SPLIT_SQUAT_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_BULGARIAN_SPLIT_SQUAT_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_BULGARIAN_SPLIT_SQUAT_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingBulgarianSplitSquatP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_BULGARIAN_SPLIT_SQUAT_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: BULGARIAN_SPLIT_SQUAT_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_BULGARIAN_SPLIT_SQUAT_P1_FAILED");
            }
        }
    }

    // ✅ 15. SHOULDER_PRESS_P1
    else if(command.startsWith("SAVE_TRAINING_SHOULDER_PRESS_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingShoulderPressP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_SHOULDER_PRESS_P1_SUCCESS");
                qDebug() << "✅ SAVED: SHOULDER_PRESS_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_SHOULDER_PRESS_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_SHOULDER_PRESS_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingShoulderPressP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_SHOULDER_PRESS_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: SHOULDER_PRESS_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_SHOULDER_PRESS_P1_FAILED");
            }
        }
    }

    // ✅ 16. CABLE_FLY_P1
    else if(command.startsWith("SAVE_TRAINING_CABLE_FLY_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingCableFlyP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_CABLE_FLY_P1_SUCCESS");
                qDebug() << "✅ SAVED: CABLE_FLY_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_CABLE_FLY_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CABLE_FLY_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;
            UserManagers.GetSaveTrainingCableFlyP1(user_id, approach1, approach2, approach3);
            // Нет записи = нули, всегда SUCCESS
            SendToClient(currentSocket, QString("LOAD_TRAINING_CABLE_FLY_P1_SUCCESS|%1|%2|%3")
                                            .arg(approach1).arg(approach2).arg(approach3));
            qDebug() << "✅ LOADED: CABLE_FLY_P1 -" << approach1 << approach2 << approach3;
        }
    }

    // ✅ 17. LEG_EXTENSION_P1
    else if(command.startsWith("SAVE_TRAINING_LEG_EXTENSION_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingLegExtensionP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_LEG_EXTENSION_P1_SUCCESS");
                qDebug() << "✅ SAVED: LEG_EXTENSION_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_LEG_EXTENSION_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_LEG_EXTENSION_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingLegExtensionP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_LEG_EXTENSION_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: LEG_EXTENSION_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_LEG_EXTENSION_P1_FAILED");
            }
        }
    }

    // ✅ 18. PLANK_P1
    else if(command.startsWith("SAVE_TRAINING_PLANK_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingPlankP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_PLANK_P1_SUCCESS");
                qDebug() << "✅ SAVED: PLANK_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_PLANK_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_PLANK_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingPlankP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_PLANK_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: PLANK_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_PLANK_P1_FAILED");
            }
        }
    }

    // ✅ 19. CHEST_FLY_P1
    else if(command.startsWith("SAVE_TRAINING_CHEST_FLY_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float approach1 = parts.at(2).toFloat();
            float approach2 = parts.at(3).toFloat();
            float approach3 = parts.at(4).toFloat();

            if(UserManagers.saveTrainingChestFlyP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, "SAVE_TRAINING_CHEST_FLY_P1_SUCCESS");
                qDebug() << "✅ SAVED: CHEST_FLY_P1";
            } else {
                SendToClient(currentSocket, "SAVE_TRAINING_CHEST_FLY_P1_FAILED");
            }
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CHEST_FLY_P1|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            float approach1 = 0, approach2 = 0, approach3 = 0;

            if(UserManagers.GetSaveTrainingChestFlyP1(user_id, approach1, approach2, approach3)) {
                SendToClient(currentSocket, QString("LOAD_TRAINING_CHEST_FLY_P1_SUCCESS|%1|%2|%3")
                                 .arg(approach1).arg(approach2).arg(approach3));
                qDebug() << "✅ LOADED: CHEST_FLY_P1 -" << approach1 << approach2 << approach3;
            } else {
                SendToClient(currentSocket, "LOAD_TRAINING_CHEST_FLY_P1_FAILED");
            }
        }
    }

    // ✅ Plan 2 — HIP_ABDUCTION_LEAN_P2
    else if(command.startsWith("SAVE_TRAINING_HIP_ABDUCTION_LEAN_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingHipAbductionLeanP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_HIP_ABDUCTION_LEAN_P2_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_HIP_ABDUCTION_LEAN_P2_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_HIP_ABDUCTION_LEAN_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingHipAbductionLeanP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_HIP_ABDUCTION_LEAN_P2_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_HIP_ABDUCTION_LEAN_P2_FAILED");
        }
    }
    // ✅ Plan 2 — PUSHUP_P2
    else if(command.startsWith("SAVE_TRAINING_PUSHUP_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingPushupP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_PUSHUP_P2_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_PUSHUP_P2_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_PUSHUP_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingPushupP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_PUSHUP_P2_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_PUSHUP_P2_FAILED");
        }
    }
    // ✅ Plan 2 — HIP_ABDUCTION_90_P2
    else if(command.startsWith("SAVE_TRAINING_HIP_ABDUCTION_90_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingHipAbduction90P2(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_HIP_ABDUCTION_90_P2_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_HIP_ABDUCTION_90_P2_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_HIP_ABDUCTION_90_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingHipAbduction90P2(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_HIP_ABDUCTION_90_P2_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_HIP_ABDUCTION_90_P2_FAILED");
        }
    }
    // ✅ Plan 2 — PULLUP_P2
    else if(command.startsWith("SAVE_TRAINING_PULLUP_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingPullupP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_PULLUP_P2_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_PULLUP_P2_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_PULLUP_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingPullupP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_PULLUP_P2_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_PULLUP_P2_FAILED");
        }
    }
    // ✅ Plan 2 — LEG_EXTENSION_FROG_P2
    else if(command.startsWith("SAVE_TRAINING_LEG_EXTENSION_FROG_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingLegExtensionFrogP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_LEG_EXTENSION_FROG_P2_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_LEG_EXTENSION_FROG_P2_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_LEG_EXTENSION_FROG_P2|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingLegExtensionFrogP2(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_LEG_EXTENSION_FROG_P2_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_LEG_EXTENSION_FROG_P2_FAILED");
        }
    }

    // =====================================================================
    // ✅ PLAN 3 — 9 новых упражнений
    // =====================================================================

    // CRANE_P3
    else if(command.startsWith("SAVE_TRAINING_CRANE_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingCraneP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_CRANE_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_CRANE_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CRANE_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingCraneP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_CRANE_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_CRANE_P3_FAILED");
        }
    }
    // SHOULDER_SIDE_RAISE_SIT_P3
    else if(command.startsWith("SAVE_TRAINING_SHOULDER_SIDE_RAISE_SIT_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingShoulderSideRaiseSitP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_SHOULDER_SIDE_RAISE_SIT_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_SHOULDER_SIDE_RAISE_SIT_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_SHOULDER_SIDE_RAISE_SIT_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingShoulderSideRaiseSitP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_SHOULDER_SIDE_RAISE_SIT_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_SHOULDER_SIDE_RAISE_SIT_P3_FAILED");
        }
    }
    // GOODMORNING_HACK_P3
    else if(command.startsWith("SAVE_TRAINING_GOODMORNING_HACK_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingGoodmorningHackP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_GOODMORNING_HACK_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_GOODMORNING_HACK_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_GOODMORNING_HACK_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingGoodmorningHackP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_GOODMORNING_HACK_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_GOODMORNING_HACK_P3_FAILED");
        }
    }
    // SHOULDER_RAISE_ONE_HAND_BENCH_P3
    else if(command.startsWith("SAVE_TRAINING_SHOULDER_RAISE_ONE_HAND_BENCH_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingShoulderRaiseOneHandBenchP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_SHOULDER_RAISE_ONE_HAND_BENCH_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_SHOULDER_RAISE_ONE_HAND_BENCH_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_SHOULDER_RAISE_ONE_HAND_BENCH_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingShoulderRaiseOneHandBenchP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_SHOULDER_RAISE_ONE_HAND_BENCH_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_SHOULDER_RAISE_ONE_HAND_BENCH_P3_FAILED");
        }
    }
    // FROGGY_STAND_P3
    else if(command.startsWith("SAVE_TRAINING_FROGGY_STAND_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingFroggyStandP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_FROGGY_STAND_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_FROGGY_STAND_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_FROGGY_STAND_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingFroggyStandP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_FROGGY_STAND_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_FROGGY_STAND_P3_FAILED");
        }
    }
    // HACK_SQUAT_P3
    else if(command.startsWith("SAVE_TRAINING_HACK_SQUAT_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingHackSquatP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_HACK_SQUAT_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_HACK_SQUAT_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_HACK_SQUAT_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingHackSquatP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_HACK_SQUAT_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_HACK_SQUAT_P3_FAILED");
        }
    }
    // PULLOVER_P3
    else if(command.startsWith("SAVE_TRAINING_PULLOVER_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingPulloverP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_PULLOVER_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_PULLOVER_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_PULLOVER_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingPulloverP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_PULLOVER_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_PULLOVER_P3_FAILED");
        }
    }
    // CROSSOVER_DIAGONAL_KICK_P3
    else if(command.startsWith("SAVE_TRAINING_CROSSOVER_DIAGONAL_KICK_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingCrossoverDiagonalKickP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_CROSSOVER_DIAGONAL_KICK_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_CROSSOVER_DIAGONAL_KICK_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CROSSOVER_DIAGONAL_KICK_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingCrossoverDiagonalKickP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_CROSSOVER_DIAGONAL_KICK_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_CROSSOVER_DIAGONAL_KICK_P3_FAILED");
        }
    }
    // WALL_ABDUCTION_ONE_P3
    else if(command.startsWith("SAVE_TRAINING_WALL_ABDUCTION_ONE_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingWallAbductionOneP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_WALL_ABDUCTION_ONE_P3_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_WALL_ABDUCTION_ONE_P3_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_WALL_ABDUCTION_ONE_P3|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingWallAbductionOneP3(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_WALL_ABDUCTION_ONE_P3_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_WALL_ABDUCTION_ONE_P3_FAILED");
        }
    }

    // =====================================================================
    // ✅ PLAN 4 — 6 новых упражнений
    // =====================================================================

    // CABLE_KICKBACK_P4
    else if(command.startsWith("SAVE_TRAINING_CABLE_KICKBACK_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingCableKickbackP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_CABLE_KICKBACK_P4_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_CABLE_KICKBACK_P4_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CABLE_KICKBACK_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingCableKickbackP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_CABLE_KICKBACK_P4_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_CABLE_KICKBACK_P4_FAILED");
        }
    }
    // CROSSOVER_STEP_UP_P4
    else if(command.startsWith("SAVE_TRAINING_CROSSOVER_STEP_UP_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingCrossoverStepUpP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_CROSSOVER_STEP_UP_P4_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_CROSSOVER_STEP_UP_P4_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CROSSOVER_STEP_UP_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingCrossoverStepUpP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_CROSSOVER_STEP_UP_P4_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_CROSSOVER_STEP_UP_P4_FAILED");
        }
    }
    // DIPS_P4
    else if(command.startsWith("SAVE_TRAINING_DIPS_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingDipsP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_DIPS_P4_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_DIPS_P4_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_DIPS_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingDipsP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_DIPS_P4_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_DIPS_P4_FAILED");
        }
    }
    // CABLE_STORK_P4
    else if(command.startsWith("SAVE_TRAINING_CABLE_STORK_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingCableStorkP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_CABLE_STORK_P4_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_CABLE_STORK_P4_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CABLE_STORK_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingCableStorkP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_CABLE_STORK_P4_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_CABLE_STORK_P4_FAILED");
        }
    }
    // GLUTE_BRIDGE_SINGLE_LEG_P4
    else if(command.startsWith("SAVE_TRAINING_GLUTE_BRIDGE_SINGLE_LEG_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingGluteBridgeSingleLegP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_GLUTE_BRIDGE_SINGLE_LEG_P4_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_GLUTE_BRIDGE_SINGLE_LEG_P4_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_GLUTE_BRIDGE_SINGLE_LEG_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingGluteBridgeSingleLegP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_GLUTE_BRIDGE_SINGLE_LEG_P4_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_GLUTE_BRIDGE_SINGLE_LEG_P4_FAILED");
        }
    }
    // CABLE_ROTATION_P4
    else if(command.startsWith("SAVE_TRAINING_CABLE_ROTATION_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingCableRotationP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_CABLE_ROTATION_P4_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_CABLE_ROTATION_P4_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_CABLE_ROTATION_P4|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingCableRotationP4(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_CABLE_ROTATION_P4_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_CABLE_ROTATION_P4_FAILED");
        }
    }

    // =====================================================================
    // ✅ PLAN 7 — 4 новых упражнения
    // =====================================================================

    // NEGATIVE_PULLUP_P7
    else if(command.startsWith("SAVE_TRAINING_NEGATIVE_PULLUP_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingNegativePullupP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_NEGATIVE_PULLUP_P7_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_NEGATIVE_PULLUP_P7_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_NEGATIVE_PULLUP_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingNegativePullupP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_NEGATIVE_PULLUP_P7_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_NEGATIVE_PULLUP_P7_FAILED");
        }
    }
    // BARBELL_BICEP_CURL_P7
    else if(command.startsWith("SAVE_TRAINING_BARBELL_BICEP_CURL_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingBarbellBicepCurlP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_BARBELL_BICEP_CURL_P7_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_BARBELL_BICEP_CURL_P7_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_BARBELL_BICEP_CURL_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingBarbellBicepCurlP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_BARBELL_BICEP_CURL_P7_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_BARBELL_BICEP_CURL_P7_FAILED");
        }
    }
    // LEG_RAISE_ELBOW_P7
    else if(command.startsWith("SAVE_TRAINING_LEG_RAISE_ELBOW_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingLegRaiseElbowP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_LEG_RAISE_ELBOW_P7_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_LEG_RAISE_ELBOW_P7_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_LEG_RAISE_ELBOW_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingLegRaiseElbowP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_LEG_RAISE_ELBOW_P7_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_LEG_RAISE_ELBOW_P7_FAILED");
        }
    }
    // BRACHIALIS_CURL_P7
    else if(command.startsWith("SAVE_TRAINING_BRACHIALIS_CURL_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 5) {
            int user_id = parts.at(1).toInt();
            float a1=parts.at(2).toFloat(), a2=parts.at(3).toFloat(), a3=parts.at(4).toFloat();
            if(UserManagers.saveTrainingBrachialiscurlP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,"SAVE_TRAINING_BRACHIALIS_CURL_P7_SUCCESS");
            else SendToClient(currentSocket,"SAVE_TRAINING_BRACHIALIS_CURL_P7_FAILED");
        }
    }
    else if(command.startsWith("LOAD_TRAINING_BRACHIALIS_CURL_P7|")) {
        QStringList parts = command.split("|");
        if(parts.size() >= 2) {
            int user_id=parts.at(1).toInt(); float a1=0,a2=0,a3=0;
            if(UserManagers.GetSaveTrainingBrachialiscurlP7(user_id,a1,a2,a3))
                SendToClient(currentSocket,QString("LOAD_TRAINING_BRACHIALIS_CURL_P7_SUCCESS|%1|%2|%3").arg(a1).arg(a2).arg(a3));
            else SendToClient(currentSocket,"LOAD_TRAINING_BRACHIALIS_CURL_P7_FAILED");
        }
    }

    // ✅ ПРОГРЕСС ТРЕНИРОВОК
    else if (command.startsWith("GET_WORKOUT_PROGRESS|")) {
        handleGetWorkoutProgress(currentSocket, command);
    }
    else if (command.startsWith("MARK_WORKOUT_DONE|")) {
        handleMarkWorkoutDone(currentSocket, command);
    }

    // ✅ СТАТИСТИКА ТРЕНИРОВОК
    else if (command.startsWith("GET_WORKOUT_STATS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int uid = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.getWorkoutStats(uid, jsonData)) {
                SendToClient(currentSocket, "WORKOUT_STATS_DATA|" + jsonData);
            } else {
                SendToClient(currentSocket, "WORKOUT_STATS_FAILED|db_error");
            }
        } else {
            SendToClient(currentSocket, "WORKOUT_STATS_FAILED|bad_format");
        }
    }

    // ✅ СБРОС ПРОГРЕССА ТРЕНИРОВОК
    else if (command.startsWith("RESET_WORKOUT_PROGRESS|")) {
        QStringList parts = command.split("|");
        if (parts.size() < 2) {
            SendToClient(currentSocket, "RESET_WORKOUT_PROGRESS_FAILED|bad_format");
        } else {
            int user_id = parts.at(1).toInt();
            if (UserManagers.resetWorkoutProgress(user_id)) {
                SendToClient(currentSocket, "RESET_WORKOUT_PROGRESS_SUCCESS");
                qDebug() << "✅ RESET_WORKOUT_PROGRESS: user" << user_id;
            } else {
                SendToClient(currentSocket, "RESET_WORKOUT_PROGRESS_FAILED|db_error");
            }
        }
    }

    // ✅ GET_RECIPES — клиент запрашивает список всех рецептов
    else if (command.startsWith("GET_RECIPES")) {
        qDebug() << "GET_RECIPES requested";
        QString jsonData;
        if (UserManagers.loadAllRecipes(jsonData)) {
            SendToClient(currentSocket, "RECIPES_DATA|" + jsonData);
        } else {
            SendToClient(currentSocket, "RECIPES_FAILED");
        }
    }

    // ✅ GET_RECIPE_DETAILS|<recipe_id> — детали одного рецепта
    else if (command.startsWith("GET_RECIPE_DETAILS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int recipeId = parts.at(1).toInt();
            qDebug() << "GET_RECIPE_DETAILS: id=" << recipeId;
            QString jsonData;
            if (UserManagers.loadRecipeDetails(recipeId, jsonData)) {
                SendToClient(currentSocket,
                             QString("RECIPE_DETAILS_DATA|%1|%2").arg(recipeId).arg(jsonData));
            } else {
                SendToClient(currentSocket, "RECIPE_DETAILS_FAILED");
            }
        }
    }

    // ✅ ADD_RECIPE|name|mealType|kcal|protein|fat|carbs|rating|ingredient|img|ingredientsJson|stepsJson
    //    Пример из клиента: ADD_RECIPE|Чиа-пудинг|Завтрак|210|8|9|24|4.5|чиа|🍮|[["Чиа","40г"]]|["Замочить","Охладить"]
    else if (command.startsWith("ADD_RECIPE|")) {
        // Используем split с ограничением, чтобы не резать JSON по вертикальной черте.
        // JSON-поля (ingredients и steps) передаём Base64, чтобы не конфликтовать с разделителем.
        //
        // Формат: ADD_RECIPE|name|mealType|kcal|protein|fat|carbs|rating|ingredient|img|<ingredients_b64>|<steps_b64>
        QStringList parts = command.split("|");
        if (parts.size() >= 12) {
            QString name          = parts.at(1);
            QString mealType      = parts.at(2);
            int     kcal          = parts.at(3).toInt();
            int     protein       = parts.at(4).toInt();
            int     fat           = parts.at(5).toInt();
            int     carbs         = parts.at(6).toInt();
            double  rating        = parts.at(7).toDouble();
            QString ingredient    = parts.at(8);
            QString img           = parts.at(9);
            // Декодируем JSON из Base64
            QString ingredientsJson = QString::fromUtf8(QByteArray::fromBase64(parts.at(10).toLatin1()));
            QString stepsJson       = QString::fromUtf8(QByteArray::fromBase64(parts.at(11).toLatin1()));

            int newId = -1;
            if (UserManagers.addRecipe(name, mealType, kcal, protein, fat, carbs,
                                       rating, ingredient, img,
                                       ingredientsJson, stepsJson, newId)) {
                SendToClient(currentSocket, QString("ADD_RECIPE_SUCCESS|%1").arg(newId));
                qDebug() << "ADD_RECIPE: created id=" << newId << name;
            } else {
                SendToClient(currentSocket, "ADD_RECIPE_FAILED");
                qDebug() << "ADD_RECIPE: failed for" << name;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
            qDebug() << "ADD_RECIPE: bad format, parts=" << parts.size();
        }
    }

    // ✅ SAVE_REVIEW|recipe_id|user_id|user_name|rating|comment_b64
    else if (command.startsWith("SAVE_REVIEW|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 6) {
            int     recipe_id = parts.at(1).toInt();
            int     user_id   = parts.at(2).toInt();
            QString user_name = parts.at(3);
            int     rating    = parts.at(4).toInt();
            // comment закодирован в base64 чтобы "|" не ломал парсинг
            QString comment   = QString::fromUtf8(QByteArray::fromBase64(parts.at(5).toLatin1()));

            if (UserManagers.saveReview(recipe_id, user_id, user_name, rating, comment)) {
                QString reviewsJson;
                UserManagers.loadReviews(recipe_id, reviewsJson);
                SendToClient(currentSocket, QString("SAVE_REVIEW_SUCCESS|%1|%2").arg(recipe_id).arg(reviewsJson));
                // Проверяем достижения за отзывы
                {
                    QString newAch;
                    UserManagers.checkAndGrantAchievements(user_id, newAch);
                    if (!newAch.isEmpty() && newAch != "[]")
                        SendToClient(currentSocket, "ACHIEVEMENTS_NEW|" + newAch);
                }
                qDebug() << "SAVE_REVIEW: recipe=" << recipe_id << "user=" << user_id;
            } else {
                SendToClient(currentSocket, "SAVE_REVIEW_FAILED");
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // ✅ GET_REVIEWS|recipe_id
    else if (command.startsWith("GET_REVIEWS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int recipe_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.loadReviews(recipe_id, jsonData)) {
                SendToClient(currentSocket, QString("REVIEWS_DATA|%1|%2").arg(recipe_id).arg(jsonData));
                qDebug() << "GET_REVIEWS: recipe=" << recipe_id;
            } else {
                SendToClient(currentSocket, "GET_REVIEWS_FAILED");
            }
        }
    }
    // SAVE_DAILY_NUTRITION|user_id|date|kcal|protein|fat|carbs|meals_json_base64
    else if (command.startsWith("SAVE_DAILY_NUTRITION|")) {
        QStringList parts = command.split("|");
        // parts: [0]=cmd, [1]=uid, [2]=date, [3]=kcal, [4]=prot, [5]=fat, [6]=carbs, [7..]=mealsJson (may contain |)
        if (parts.size() >= 8) {
            int     user_id = parts.at(1).toInt();
            QString date    = parts.at(2);
            int     kcal    = parts.at(3).toInt();
            int     protein = parts.at(4).toInt();
            int     fat     = parts.at(5).toInt();
            int     carbs   = parts.at(6).toInt();
            // Всё остальное — JSON блюд (может содержать '|')
            QStringList jsonParts = parts.mid(7);
            QString mealsJson = jsonParts.join("|");

            if (UserManagers.saveDailyNutrition(user_id, date, mealsJson, kcal, protein, fat, carbs)) {
                SendToClient(currentSocket, "SAVE_DAILY_NUTRITION_OK");
                qDebug() << "Daily nutrition saved for user" << user_id << "date" << date;
            } else {
                SendToClient(currentSocket, "SAVE_DAILY_NUTRITION_FAIL");
                qDebug() << "Failed to save daily nutrition for user" << user_id;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // GET_DAILY_NUTRITION|user_id|date
    else if (command.startsWith("GET_DAILY_NUTRITION|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int     user_id = parts.at(1).toInt();
            QString date    = parts.at(2);

            QString mealsJson;
            int kcal = 0, protein = 0, fat = 0, carbs = 0;

            if (UserManagers.loadDailyNutrition(user_id, date, mealsJson, kcal, protein, fat, carbs)) {
                // Ответ: GET_DAILY_NUTRITION_OK|date|kcal|protein|fat|carbs|mealsJson
                QString response = QString("GET_DAILY_NUTRITION_OK|%1|%2|%3|%4|%5|%6")
                                       .arg(date).arg(kcal).arg(protein).arg(fat).arg(carbs)
                                       .arg(mealsJson);
                SendToClient(currentSocket, response);
                qDebug() << "Daily nutrition loaded for user" << user_id << "date" << date;
            } else {
                SendToClient(currentSocket, "GET_DAILY_NUTRITION_FAIL");
                qDebug() << "Failed to load daily nutrition for user" << user_id << "date" << date;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // GET_NUTRITION_HISTORY|user_id
    else if (command.startsWith("GET_NUTRITION_HISTORY|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;

            if (UserManagers.loadNutritionHistory(user_id, jsonData)) {
                SendToClient(currentSocket, "GET_NUTRITION_HISTORY_OK|" + jsonData);
                qDebug() << "Nutrition history sent for user" << user_id;
            } else {
                SendToClient(currentSocket, "GET_NUTRITION_HISTORY_FAIL");
                qDebug() << "Failed to load nutrition history for user" << user_id;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // ✅ SAVE_PROGRAM|user_id|planInt
    // Сохраняет выбранный план тренировок в колонку PlanTrainning таблицы USERS
    // planInt: 0 = не выбрано, 1 = gym_1 (добавляйте новые значения при расширении)
    else if (command.startsWith("SAVE_PROGRAM|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            int planInt = parts.at(2).toInt();

            if (UserManagers.saveEPlanTrainningUserData(user_id, planInt)) {
                if (planInt > 0) {
                    // Новый план — фиксируем точное время начала (нужно для тестового режима)
                    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
                    UserManagers.savePlanStartDate(user_id, now);
                    qDebug() << "✅ Plan start date set:" << now;
                } else {
                    // Очистка плана — сбрасываем дату начала
                    UserManagers.savePlanStartDate(user_id, "");
                }
                SendToClient(currentSocket, "SAVE_PROGRAM_SUCCESS");
                qDebug() << "✅ SAVE_PROGRAM: user=" << user_id << "plan=" << planInt;
            } else {
                SendToClient(currentSocket, "SAVE_PROGRAM_FAILED");
                qDebug() << "❌ SAVE_PROGRAM: failed for user=" << user_id;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
            qDebug() << "SAVE_PROGRAM: bad format";
        }
    }

    // SAVE_EXERCISE_SUBSTITUTION|user_id|original_name|substitute_name
    // substitute_name пустой = сбросить замену
    else if (command.startsWith("SAVE_EXERCISE_SUBSTITUTION|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 4) {
            int     user_id       = parts.at(1).toInt();
            QString originalName  = parts.at(2);
            QString substituteName = parts.at(3);

            if (UserManagers.saveExerciseSubstitution(user_id, originalName, substituteName)) {
                SendToClient(currentSocket,
                             "SAVE_EXERCISE_SUBSTITUTION_OK|" + originalName + "|" + substituteName);
                qDebug() << "✅ SAVE_EXERCISE_SUBSTITUTION: user=" << user_id
                         << originalName << "->" << substituteName;
            } else {
                SendToClient(currentSocket, "SAVE_EXERCISE_SUBSTITUTION_FAIL");
                qDebug() << "❌ SAVE_EXERCISE_SUBSTITUTION failed for user=" << user_id;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // LOAD_EXERCISE_SUBSTITUTIONS|user_id
    else if (command.startsWith("LOAD_EXERCISE_SUBSTITUTIONS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int     user_id  = parts.at(1).toInt();
            QString jsonData;

            if (UserManagers.loadExerciseSubstitutions(user_id, jsonData)) {
                SendToClient(currentSocket, "LOAD_EXERCISE_SUBSTITUTIONS_OK|" + jsonData);
                qDebug() << "✅ LOAD_EXERCISE_SUBSTITUTIONS: user=" << user_id;
            } else {
                SendToClient(currentSocket, "LOAD_EXERCISE_SUBSTITUTIONS_FAIL");
                qDebug() << "❌ LOAD_EXERCISE_SUBSTITUTIONS failed for user=" << user_id;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // ==================== АДМИН-ПАНЕЛЬ ====================

    // ADMIN_GET_ALL_USERS
    else if (command == "ADMIN_GET_ALL_USERS") {
        QString jsonData;
        if (UserManagers.getAllUsers(jsonData)) {
            SendToClient(currentSocket, "ADMIN_USERS_DATA|" + jsonData);
            qDebug() << "ADMIN_GET_ALL_USERS: OK";
        } else {
            SendToClient(currentSocket, "ADMIN_USERS_FAILED");
            qDebug() << "ADMIN_GET_ALL_USERS: FAILED";
        }
    }

    // ADMIN_SET_SUBSCRIPTION|user_id|0or1
    else if (command.startsWith("ADMIN_SET_SUBSCRIPTION|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            int status  = parts.at(2).toInt();
            if (UserManagers.setSubscription(user_id, status)) {
                SendToClient(currentSocket, QString("ADMIN_SUBSCRIPTION_OK|%1|%2").arg(user_id).arg(status));
                qDebug() << "ADMIN_SET_SUBSCRIPTION: user=" << user_id << "status=" << status;
            } else {
                SendToClient(currentSocket, "ADMIN_SUBSCRIPTION_FAILED");
            }
        }
    }

    // ADMIN_DELETE_RECIPE|recipe_id
    else if (command.startsWith("ADMIN_DELETE_RECIPE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int recipe_id = parts.at(1).toInt();
            if (UserManagers.deleteRecipe(recipe_id)) {
                SendToClient(currentSocket, QString("ADMIN_DELETE_RECIPE_OK|%1").arg(recipe_id));
                qDebug() << "ADMIN_DELETE_RECIPE: id=" << recipe_id;
            } else {
                SendToClient(currentSocket, "ADMIN_DELETE_RECIPE_FAILED");
            }
        }
    }

    // ADMIN_GET_RECIPES — загрузить рецепты для админки
    else if (command == "ADMIN_GET_RECIPES") {
        QString jsonData;
        if (UserManagers.loadAllRecipes(jsonData)) {
            SendToClient(currentSocket, "ADMIN_RECIPES_DATA|" + jsonData);
            qDebug() << "ADMIN_GET_RECIPES: OK";
        } else {
            SendToClient(currentSocket, "ADMIN_RECIPES_FAILED");
        }
    }

    // ADMIN_ADD_RECIPE|name|mealType|kcal|protein|fat|carbs|rating|ingredient|img_b64|ingredients_b64|steps_b64
    else if (command.startsWith("ADMIN_ADD_RECIPE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 12) {
            QString name      = parts.at(1);
            QString mealType  = parts.at(2);
            int kcal          = parts.at(3).toInt();
            int protein       = parts.at(4).toInt();
            int fat           = parts.at(5).toInt();
            int carbs         = parts.at(6).toInt();
            double rating     = parts.at(7).toDouble();
            QString ingredient = parts.at(8);
            QString img       = parts.at(9);
            QString ingredientsJson = QString::fromUtf8(QByteArray::fromBase64(parts.at(10).toLatin1()));
            QString stepsJson       = QString::fromUtf8(QByteArray::fromBase64(parts.at(11).toLatin1()));

            int newId;
            if (UserManagers.addRecipe(name, mealType, kcal, protein, fat, carbs,
                                        rating, ingredient, img, ingredientsJson, stepsJson, newId)) {
                SendToClient(currentSocket, QString("ADMIN_ADD_RECIPE_OK|%1").arg(newId));
                qDebug() << "ADMIN_ADD_RECIPE: newId=" << newId;
            } else {
                SendToClient(currentSocket, "ADMIN_ADD_RECIPE_FAILED");
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }

    // ADMIN_GET_REVIEWS|recipe_id
    else if (command.startsWith("ADMIN_GET_REVIEWS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int recipe_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.loadReviews(recipe_id, jsonData)) {
                SendToClient(currentSocket, QString("ADMIN_REVIEWS_DATA|%1|%2").arg(recipe_id).arg(jsonData));
                qDebug() << "ADMIN_GET_REVIEWS: recipe=" << recipe_id;
            } else {
                SendToClient(currentSocket, "ADMIN_REVIEWS_FAILED");
            }
        }
    }

    // ADMIN_DELETE_REVIEW|review_id|recipe_id
    else if (command.startsWith("ADMIN_DELETE_REVIEW|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int review_id = parts.at(1).toInt();
            int recipe_id = parts.at(2).toInt();
            if (UserManagers.deleteReview(review_id, recipe_id)) {
                // Возвращаем обновлённый список отзывов
                QString jsonData;
                UserManagers.loadReviews(recipe_id, jsonData);
                SendToClient(currentSocket, QString("ADMIN_DELETE_REVIEW_OK|%1|%2").arg(recipe_id).arg(jsonData));
                qDebug() << "ADMIN_DELETE_REVIEW: reviewId=" << review_id << "recipeId=" << recipe_id;
            } else {
                SendToClient(currentSocket, "ADMIN_DELETE_REVIEW_FAILED");
            }
        }
    }

    // ADMIN_GET_USER_WORKOUT|user_id
    else if (command.startsWith("ADMIN_GET_USER_WORKOUT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.getUserWorkoutInfo(user_id, jsonData)) {
                SendToClient(currentSocket, QString("ADMIN_USER_WORKOUT_DATA|%1|%2").arg(user_id).arg(jsonData));
                qDebug() << "ADMIN_GET_USER_WORKOUT: userId=" << user_id;
            } else {
                SendToClient(currentSocket, "ADMIN_USER_WORKOUT_FAILED");
            }
        }
    }

    // ADMIN_RESET_USER_PLAN|user_id
    else if (command.startsWith("ADMIN_RESET_USER_PLAN|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (UserManagers.adminResetUserPlan(user_id)) {
                SendToClient(currentSocket, QString("ADMIN_RESET_PLAN_OK|%1").arg(user_id));
                qDebug() << "ADMIN_RESET_USER_PLAN: userId=" << user_id;
            } else {
                SendToClient(currentSocket, "ADMIN_RESET_PLAN_FAILED");
            }
        }
    }

    // ==================== ДРУЗЬЯ ====================

    // GETFRIENDID|user_id → FRIENDID|friendId
    else if (command.startsWith("GETFRIENDID|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            int friendId = 0;
            if (UserManagers.getUserFriendId(user_id, friendId)) {
                SendToClient(currentSocket, QString("FRIENDID|%1").arg(friendId));
                qDebug() << "GETFRIENDID: user=" << user_id << "friendId=" << friendId;
            } else {
                SendToClient(currentSocket, "GETFRIENDID_FAILED");
            }
        }
    }
    // SEARCHFRIEND|my_user_id|target_friend_id → SEARCHFRIEND_OK|json or SEARCHFRIEND_NOTFOUND
    else if (command.startsWith("SEARCHFRIEND|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int myUserId       = parts.at(1).toInt();
            int targetFriendId = parts.at(2).toInt();
            QString jsonData;
            if (UserManagers.searchUserByFriendId(myUserId, targetFriendId, jsonData)) {
                SendToClient(currentSocket, jsonData.isEmpty()
                             ? QString("SEARCHFRIEND_NOTFOUND")
                             : QString("SEARCHFRIEND_OK|") + jsonData);
            } else {
                SendToClient(currentSocket, "SEARCHFRIEND_FAILED");
            }
        }
    }
    // SENDFRIENDREQUEST|from_user_id|target_friend_id
    else if (command.startsWith("SENDFRIENDREQUEST|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int fromUserId     = parts.at(1).toInt();
            int targetFriendId = parts.at(2).toInt();
            QString err;
            int toUserId = -1;
            if (UserManagers.sendFriendRequest(fromUserId, targetFriendId, err, toUserId)) {
                SendToClient(currentSocket, "SENDFRIENDREQUEST_OK");
                // Пушим уведомление целевому пользователю если он онлайн
                if (toUserId != -1) {
                    QTcpSocket* targetSocket = findSocketByUserId(toUserId);
                    if (targetSocket) {
                        QString requestsJson;
                        UserManagers.getIncomingFriendRequests(toUserId, requestsJson);
                        SendToClient(targetSocket, "FRIENDREQUESTS_DATA|" + requestsJson);
                        qDebug() << "📨 Pushed friend request notification to user" << toUserId;
                    }
                }
            } else {
                SendToClient(currentSocket, "SENDFRIENDREQUEST_FAILED|" + err);
            }
        }
    }
    // GETFRIENDREQUESTS|user_id
    else if (command.startsWith("GETFRIENDREQUESTS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.getIncomingFriendRequests(user_id, jsonData)) {
                SendToClient(currentSocket, "FRIENDREQUESTS_DATA|" + jsonData);
            } else {
                SendToClient(currentSocket, "FRIENDREQUESTS_FAILED");
            }
        }
    }
    // ACCEPTFRIENDREQUEST|my_user_id|request_id
    else if (command.startsWith("ACCEPTFRIENDREQUEST|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int myUserId  = parts.at(1).toInt();
            int requestId = parts.at(2).toInt();
            if (UserManagers.acceptFriendRequest(myUserId, requestId)) {
                SendToClient(currentSocket, "ACCEPTFRIENDREQUEST_OK");
                // Проверяем достижения за друзей
                {
                    QString newAch;
                    UserManagers.checkAndGrantAchievements(myUserId, newAch);
                    if (!newAch.isEmpty() && newAch != "[]")
                        SendToClient(currentSocket, "ACHIEVEMENTS_NEW|" + newAch);
                }
            } else {
                SendToClient(currentSocket, "ACCEPTFRIENDREQUEST_FAILED");
            }
        }
    }
    // DECLINEFRIENDREQUEST|my_user_id|request_id
    else if (command.startsWith("DECLINEFRIENDREQUEST|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int myUserId  = parts.at(1).toInt();
            int requestId = parts.at(2).toInt();
            if (UserManagers.declineFriendRequest(myUserId, requestId)) {
                SendToClient(currentSocket, "DECLINEFRIENDREQUEST_OK");
            } else {
                SendToClient(currentSocket, "DECLINEFRIENDREQUEST_FAILED");
            }
        }
    }
    // GETFRIENDS|user_id
    else if (command.startsWith("GETFRIENDS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.getFriends(user_id, jsonData)) {
                SendToClient(currentSocket, "FRIENDS_DATA|" + jsonData);
            } else {
                SendToClient(currentSocket, "FRIENDS_FAILED");
            }
        }
    }
    // REMOVEFRIEND|my_user_id|friend_user_id
    else if (command.startsWith("REMOVEFRIEND|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int myUserId     = parts.at(1).toInt();
            int friendUserId = parts.at(2).toInt();
            if (UserManagers.removeFriend(myUserId, friendUserId)) {
                SendToClient(currentSocket, "REMOVEFRIEND_OK");
            } else {
                SendToClient(currentSocket, "REMOVEFRIEND_FAILED");
            }
        }
    }

    // ADMIN_GET_LEADERBOARD — топ 100 по кач-баллам
    else if (command == "ADMIN_GET_LEADERBOARD") {
        QString jsonData;
        if (UserManagers.getLeaderboard(jsonData))
            SendToClient(currentSocket, "ADMIN_LEADERBOARD_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_LEADERBOARD_FAILED");
    }

    // ADMIN_RESET_ALL_KACHBALLS — обнулить кач-баллы всех пользователей
    else if (command == "ADMIN_RESET_ALL_KACHBALLS") {
        if (UserManagers.resetAllKachballs()) {
            SendToClient(currentSocket, "ADMIN_RESET_KACHBALLS_OK");
            qDebug() << "ADMIN_RESET_ALL_KACHBALLS: OK";
        } else {
            SendToClient(currentSocket, "ADMIN_RESET_KACHBALLS_FAILED");
        }
    }

    // ADMIN_GET_GAMIFICATION — получить состояние геймификации
    else if (command == "ADMIN_GET_GAMIFICATION") {
        QString jsonData;
        if (UserManagers.getGamificationStatus(jsonData))
            SendToClient(currentSocket, "ADMIN_GAMIFICATION_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_GAMIFICATION_FAILED");
    }

    // ADMIN_SET_GAMIFICATION_DAY|day
    else if (command.startsWith("ADMIN_SET_GAMIFICATION_DAY|")) {
        int day = command.split("|").at(1).toInt();
        QString jsonData;
        if (UserManagers.setGamificationDay(day) && UserManagers.getGamificationStatus(jsonData))
            SendToClient(currentSocket, "ADMIN_GAMIFICATION_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_GAMIFICATION_FAILED");
    }

    // ADMIN_RESET_GAMIFICATION — сбросить геймификацию на день 1
    else if (command == "ADMIN_RESET_GAMIFICATION") {
        QString jsonData;
        if (UserManagers.resetGamification() && UserManagers.getGamificationStatus(jsonData))
            SendToClient(currentSocket, "ADMIN_GAMIFICATION_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_GAMIFICATION_FAILED");
    }

    // ==================== ДОСТИЖЕНИЯ И КАЧБАЛЛЫ ====================

    // GET_ACHIEVEMENTS|user_id → ACHIEVEMENTS_DATA|json
    else if (command.startsWith("GET_ACHIEVEMENTS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.getUserAchievements(user_id, jsonData)) {
                SendToClient(currentSocket, "ACHIEVEMENTS_DATA|" + jsonData);
                qDebug() << "Achievements loaded for user" << user_id;
            } else {
                SendToClient(currentSocket, "ACHIEVEMENTS_FAILED");
            }
        }
    }
    // CHECK_ACHIEVEMENTS|user_id → ACHIEVEMENTS_NEW|json (новые достижения)
    else if (command.startsWith("CHECK_ACHIEVEMENTS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString newJson;
            if (UserManagers.checkAndGrantAchievements(user_id, newJson)) {
                SendToClient(currentSocket, "ACHIEVEMENTS_NEW|" + newJson);
                qDebug() << "Achievements checked for user" << user_id;
            } else {
                SendToClient(currentSocket, "ACHIEVEMENTS_CHECK_FAILED");
            }
        }
    }
    // GET_KACHBALLS|user_id → KACHBALLS|count
    else if (command.startsWith("GET_KACHBALLS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            int kachballs = 0;
            if (UserManagers.getUserKachballs(user_id, kachballs)) {
                SendToClient(currentSocket, QString("KACHBALLS|%1").arg(kachballs));
                qDebug() << "Kachballs for user" << user_id << ":" << kachballs;
            } else {
                SendToClient(currentSocket, "KACHBALLS_FAILED");
            }
        }
    }
    // GET_WEEKLY_MISSIONS|user_id|year|week
    else if (command.startsWith("GET_WEEKLY_MISSIONS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 4) {
            int user_id = parts.at(1).toInt();
            int year    = parts.at(2).toInt();
            int week    = parts.at(3).toInt();
            QString jsonData;
            if (UserManagers.getWeeklyMissions(user_id, year, week, jsonData)) {
                SendToClient(currentSocket, "WEEKLY_MISSIONS_DATA|" + jsonData);
            } else {
                SendToClient(currentSocket, "WEEKLY_MISSIONS_FAILED");
            }
        }
    }
    // UPDATE_WEEKLY_MISSIONS|user_id|year|week|steps|workouts|nutrition
    else if (command.startsWith("UPDATE_WEEKLY_MISSIONS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 7) {
            int  user_id    = parts.at(1).toInt();
            int  year       = parts.at(2).toInt();
            int  week       = parts.at(3).toInt();
            bool steps      = parts.at(4).toInt() == 1;
            bool workouts   = parts.at(5).toInt() == 1;
            bool nutrition  = parts.at(6).toInt() == 1;
            int  awarded    = 0;
            QString jsonData;
            if (UserManagers.updateWeeklyMissions(user_id, year, week,
                                                  steps, workouts, nutrition,
                                                  awarded, jsonData)) {
                SendToClient(currentSocket, "WEEKLY_MISSIONS_DATA|" + jsonData);
            } else {
                SendToClient(currentSocket, "WEEKLY_MISSIONS_FAILED");
            }
        }
    }

    else {
        qDebug() << "Unknown command:" << command.left(50);
    }
}

void Server::handleGetWorkoutProgress(QTcpSocket* currentSocket, const QString& command)
{
    // Формат: GET_WORKOUT_PROGRESS|user_id
    QStringList parts = command.split("|");
    if (parts.size() < 2) {
        SendToClient(currentSocket, "GET_WORKOUT_PROGRESS_FAILED|bad_format");
        return;
    }
    int user_id = parts.at(1).toInt();

    // Проверяем нужно ли сбросить незавершённую неделю
    UserManagers.tryAdvanceWeek(user_id);

    int  current_week;
    bool w1, w2, w3, plan_completed;
    QString d1, d2, d3;

    if (UserManagers.getWorkoutProgress(user_id, current_week, w1, w2, w3, d1, d2, d3, plan_completed)) {
        // Если plan_start_date пустой (план выбран до обновления) — инициализируем сейчас
        QString planStartDate;
        UserManagers.getPlanStartDate(user_id, planStartDate);
        if (planStartDate.isEmpty()) {
            QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
            UserManagers.savePlanStartDate(user_id, now);
            qDebug() << "⚙️ Auto-initialized plan_start_date for user" << user_id << ":" << now;
        }

        // Сервер сам считает — прошло ли время (тест: 16 мин, реальный: 60 дней)
        bool planChangeDue = false;
        UserManagers.isPlanChangeDue(user_id, planChangeDue);

        // Формат ответа: SUCCESS|week|w1|w2|w3|plan_completed|plan_change_due
        QString resp = QString("GET_WORKOUT_PROGRESS_SUCCESS|%1|%2|%3|%4|%5|%6")
                           .arg(current_week)
                           .arg(w1 ? 1 : 0)
                           .arg(w2 ? 1 : 0)
                           .arg(w3 ? 1 : 0)
                           .arg(plan_completed ? 1 : 0)
                           .arg(planChangeDue ? 1 : 0);
        SendToClient(currentSocket, resp);
        qDebug() << "✅ GET_WORKOUT_PROGRESS:" << resp;
    } else {
        SendToClient(currentSocket, "GET_WORKOUT_PROGRESS_FAILED|db_error");
    }
}

void Server::handleMarkWorkoutDone(QTcpSocket* currentSocket, const QString& command)
{
    // Формат: MARK_WORKOUT_DONE|user_id|workout_num|date
    // workout_num: 1, 2 или 3
    QStringList parts = command.split("|");
    if (parts.size() < 4) {
        SendToClient(currentSocket, "MARK_WORKOUT_DONE_FAILED|bad_format");
        return;
    }
    int user_id    = parts.at(1).toInt();
    int workout_num = parts.at(2).toInt();
    QString date   = parts.at(3);

    if (UserManagers.markWorkoutDone(user_id, workout_num, date)) {
        // Сохраняем снапшот статистики за текущую неделю
        UserManagers.computeAndSaveWeeklyStats(user_id);

        // Возвращаем свежее состояние прогресса
        int  current_week;
        bool w1, w2, w3, plan_completed;
        QString d1, d2, d3;
        UserManagers.getWorkoutProgress(user_id, current_week, w1, w2, w3, d1, d2, d3, plan_completed);

        bool planChangeDue = false;
        UserManagers.isPlanChangeDue(user_id, planChangeDue);

        QString resp = QString("MARK_WORKOUT_DONE_SUCCESS|%1|%2|%3|%4|%5|%6")
                           .arg(current_week)
                           .arg(w1 ? 1 : 0)
                           .arg(w2 ? 1 : 0)
                           .arg(w3 ? 1 : 0)
                           .arg(plan_completed ? 1 : 0)
                           .arg(planChangeDue ? 1 : 0);
        SendToClient(currentSocket, resp);
        qDebug() << "✅ MARK_WORKOUT_DONE:" << resp;
        // Автоматически проверяем новые достижения после тренировки
        {
            QString newAch;
            UserManagers.checkAndGrantAchievements(user_id, newAch);
            if (!newAch.isEmpty() && newAch != "[]")
                SendToClient(currentSocket, "ACHIEVEMENTS_NEW|" + newAch);
        }
    } else {
        SendToClient(currentSocket, "MARK_WORKOUT_DONE_FAILED|db_error");
    }
}

int Server::findSocketIndex(QTcpSocket* sock)
{
    for(int i = 0; i < Sockets.size(); i++) {
        if(Sockets[i].socket == sock) {
            return i;
        }
    }
    return -1;
}

QTcpSocket* Server::findSocketByUserId(int userId)
{
    for (const SocketInfo &info : Sockets) {
        if (info.userId == userId && info.socket && info.socket->state() == QAbstractSocket::ConnectedState)
            return info.socket;
    }
    return nullptr;
}

bool Server::checkCredentials(const QString& login, const QString& password)
{
    return false;
}
