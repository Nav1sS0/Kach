#include "Server.h"
#include <QDebug>
#include <QDataStream>
#include <QBuffer>
#include <QDate>
#include <QDateTime>
#include <QSettings>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

Server::Server(QObject *parent)
    : QTcpServer(parent),
    inactivityCheckTimer(nullptr),
    freezeCheckTimer(nullptr),
    requireSize(0),
    comlexData(false)
{
    if (this->listen(QHostAddress::Any, 2323)) {
        qDebug() << "=== Server started on port 2323 ===";
    } else {
        qDebug() << "=== Server error: " << this->errorString() << " ===";
    }

    // ✅ Таймер проверки неактивных соединений
    inactivityCheckTimer = new QTimer(this);
    connect(inactivityCheckTimer, &QTimer::timeout, this, &Server::checkInactiveConnections);
    inactivityCheckTimer->start(5000);

    // ✅ Таймер авто-разморозки подписок (раз в час)
    freezeCheckTimer = new QTimer(this);
    connect(freezeCheckTimer, &QTimer::timeout, this, &Server::checkExpiredFreezes);
    freezeCheckTimer->start(3600 * 1000);
    QTimer::singleShot(2000, this, &Server::checkExpiredFreezes);  // прогон сразу после старта

    qDebug() << "DDoS Protection + auto-unfreeze timers started";
}

Server::~Server()
{
    if (inactivityCheckTimer) {
        inactivityCheckTimer->stop();
        inactivityCheckTimer->deleteLater();
        inactivityCheckTimer = nullptr;
    }
    if (freezeCheckTimer) {
        freezeCheckTimer->stop();
        freezeCheckTimer->deleteLater();
        freezeCheckTimer = nullptr;
    }

    // ✅ Безопасное завершение: отсоединяем сигналы перед удалением (избегаем
    // повторного вызова removeSocket из деструктора)
    for(auto& socketInfo : Sockets) {
        if(socketInfo.socket) {
            QObject::disconnect(socketInfo.socket, nullptr, this, nullptr);
            socketInfo.socket->disconnectFromHost();
            socketInfo.socket->deleteLater();
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
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    for(int i = Sockets.size() - 1; i >= 0; i--) {
        if(Sockets[i].socket == socket) {
            QString clientIp = Sockets[i].clientIp;

            // Уменьшаем счетчик для этого IP
            if (ipConnectionCount.contains(clientIp)) {
                ipConnectionCount[clientIp]--;
                if (ipConnectionCount[clientIp] <= 0) {
                    ipConnectionCount.remove(clientIp);
                    ipPacketTimestamps.remove(clientIp);
                }
            }

            Sockets.removeAt(i);
            break;
        }
    }

    // ✅ Используем deleteLater() — мы внутри слота, вызванного сигналом этого сокета.
    // delete привёл бы к use-after-free.
    socket->deleteLater();

    qDebug() << "Client disconnected. Total clients:" << Sockets.size();
}

// ✅ Helper: требует валидную авторизацию пользователя
bool Server::requireAuth(QTcpSocket* sock, const SocketInfo& info, const QString& cmdName)
{
    if (!info.authenticated || info.userId <= 0) {
        qWarning() << "🚫 Unauthorized command attempt:" << cmdName << "from IP:" << info.clientIp;
        SendToClient(sock, "UNAUTHORIZED");
        return false;
    }
    return true;
}

// ✅ Helper: проверяет, что requestedUserId совпадает с авторизованным userId
// (защита от IDOR — попыток обратиться к чужим данным)
bool Server::requireOwnership(QTcpSocket* sock, const SocketInfo& info, int requestedUserId, const QString& cmdName)
{
    if (!info.authenticated || info.userId <= 0) {
        qWarning() << "🚫 Unauthorized ownership check:" << cmdName << "from IP:" << info.clientIp;
        SendToClient(sock, "UNAUTHORIZED");
        return false;
    }
    if (info.userId != requestedUserId) {
        qWarning() << "🚫 IDOR attempt:" << cmdName
                   << "by user" << info.userId << "tried to access user" << requestedUserId
                   << "from IP:" << info.clientIp;
        SendToClient(sock, "FORBIDDEN");
        return false;
    }
    return true;
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

// ✅ Авто-разморозка: проверяем, не истёк ли срок заморозки у кого-нибудь
void Server::checkExpiredFreezes()
{
    QList<int> userIds;
    if (!UserManagers.getExpiredFreezes(userIds)) return;
    for (int uid : userIds) {
        if (UserManagers.unfreezeSubscription(uid)) {
            qDebug() << "☀ Auto-unfrozen user" << uid;
            // Если юзер онлайн — уведомим
            if (QTcpSocket* s = findSocketByUserId(uid)) {
                SendToClient(s, "SUBSCRIPTION_FROZEN_CHANGED|0");
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

    // ✅ Rate limiting — защита от flood-атак
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    ipPacketTimestamps[si.clientIp].append(nowMs);
    if (isRateLimited(si.clientIp)) {
        qWarning() << "🚫 Rate limit exceeded for IP:" << si.clientIp << "— closing connection";
        currentSocket->close();
        return;
    }

    // Читаем все доступные данные один раз
    QByteArray newData = currentSocket->readAll();
    si.bytesReceived += newData.size();

    // ✅ Используем per-socket image state (был race condition с глобальным состоянием)
    if(si.isReceivingImage && si.expectedImageSize > 0) {
        qDebug() << "Receiving image data... Current:" << si.imageBuffer.size() << "Expected:" << si.expectedImageSize;

        if(!newData.isEmpty()) {
            si.imageBuffer.append(newData);
        }

        if((quint64)si.imageBuffer.size() >= si.expectedImageSize) {
            finalizeIncomingImage(si, currentSocket);
        }
        return;
    }

    // Обработка команд — буферизация для больших пакетов
    si.buffer.append(newData);

    // ✅ Защита от memory-DoS: жёсткий лимит на размер буфера
    if (si.buffer.size() > (int)MAX_PACKET_SIZE) {
        qWarning() << "🚫 Buffer overflow attempt from IP:" << si.clientIp
                   << "(size=" << si.buffer.size() << ") — closing connection";
        currentSocket->close();
        return;
    }

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

            // ✅ Защита от подделанного размера в заголовке — DoS через allocation
            if (strByteLen > MAX_PACKET_SIZE) {
                qWarning() << "🚫 Oversized packet header from IP:" << si.clientIp
                           << "(claimed size=" << strByteLen << ") — closing connection";
                currentSocket->close();
                return;
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

        // ✅ Если команда взвела режим приёма фото — остаток si.buffer это
        // начало бинаря (фото пришло одним TCP-пакетом с заголовком). Переносим
        // его в si.imageBuffer и выходим из цикла, иначе следующая итерация
        // попытается распарсить первые 4 байта PNG как длину строки →
        // сработает анти-DoS защита и сокет будет закрыт.
        if (si.isReceivingImage && !si.buffer.isEmpty()) {
            si.imageBuffer.append(si.buffer);
            si.buffer.clear();

            // Если бинарь уже целиком в буфере — обрабатываем СРАЗУ.
            // Раньше использовался QMetaObject::invokeMethod(slotReadyRead),
            // но там sender() = nullptr → проверка currentSocket падала.
            if ((quint64)si.imageBuffer.size() >= si.expectedImageSize) {
                finalizeIncomingImage(si, currentSocket);
            }
            break;
        }
    }
}

void Server::finalizeIncomingImage(SocketInfo& si, QTcpSocket* sock)
{
    qDebug() << "Image complete! Processing... type=" << si.imagePhotoType
             << "fromAdmin=" << si.imageFromAdmin
             << "size=" << si.expectedImageSize;

    QByteArray completeImage = si.imageBuffer.left(si.expectedImageSize);
    si.imageBuffer.clear();

    if (si.imagePhotoType == "chat") {
        // ✅ Фото в чате с тренером
        qint64 chatMsgId = 0;
        const bool fromAdmin = si.imageFromAdmin;
        const int  userId    = si.imageUserId;
        if (UserManagers.sendChatPhoto(userId, fromAdmin, completeImage, chatMsgId)) {
            if (fromAdmin) {
                SendToClient(sock,
                             QString("ADMIN_CHAT_PHOTO_SENT|%1|%2").arg(userId).arg(chatMsgId));
                if (QTcpSocket *userSock = findSocketByUserId(userId)) {
                    SendToClient(userSock, "CHAT_NEW_MESSAGE");
                }
            } else {
                SendToClient(sock,
                             QString("CHAT_PHOTO_SAVED|%1").arg(chatMsgId));
            }
        } else {
            SendToClient(sock, fromAdmin ? "ADMIN_CHAT_PHOTO_FAILED"
                                          : "CHAT_PHOTO_SAVE_FAILED");
        }
    } else if (si.imagePhotoType == "support") {
        // ✅ Фото в чате техподдержки — отдельный путь
        qint64 chatMsgId = 0;
        const bool fromAdmin = si.imageFromAdmin;
        const int  userId    = si.imageUserId;
        if (UserManagers.sendSupportPhoto(userId, fromAdmin, completeImage, chatMsgId)) {
            if (fromAdmin) {
                SendToClient(sock,
                             QString("ADMIN_SUPPORT_PHOTO_SENT|%1|%2").arg(userId).arg(chatMsgId));
                if (QTcpSocket *userSock = findSocketByUserId(userId)) {
                    SendToClient(userSock, "SUPPORT_NEW_MESSAGE");
                }
            } else {
                SendToClient(sock,
                             QString("SUPPORT_PHOTO_SAVED|%1").arg(chatMsgId));
            }
        } else {
            SendToClient(sock, fromAdmin ? "ADMIN_SUPPORT_PHOTO_FAILED"
                                          : "SUPPORT_PHOTO_SAVE_FAILED");
        }
    } else if (si.imagePhotoType == "dish") {
        int dishId = 0;
        if (UserManagers.saveCookedDish(si.imageUserId, si.imageRecipeId, completeImage, dishId)) {
            int reward = UserManagers.getBonusAmount("cooked_dish", 5);
            SendToClient(sock, QString("DISH_SAVED|%1|%2").arg(dishId).arg(reward));
        } else {
            SendToClient(sock, "DISH_SAVE_FAILED");
        }
    } else if (si.imageMeasurementId > 0) {
        qDebug() << "=== saveMeasurementPhoto === User:" << si.imageUserId
                 << "Measurement:" << si.imageMeasurementId << "Type:" << si.imagePhotoType;
        if (UserManagers.saveMeasurementPhoto(si.imageUserId, si.imageMeasurementId,
                                              si.imagePhotoType, completeImage)) {
            SendToClient(sock, QString("PHOTO_SAVED|%1|%2")
                                   .arg(si.imageMeasurementId).arg(si.imagePhotoType));
        } else {
            SendToClient(sock, "PHOTO_SAVE_FAILED");
        }
    } else {
        // Аватар
        if (UserManagers.saveImage(si.imageUserId, completeImage)) {
            SendToClient(sock, "IMAGE_SAVED_SUCCESSFULLY");
        } else {
            SendToClient(sock, "DATABASE_SAVE_ERROR");
        }
    }

    si.isReceivingImage = false;
    si.expectedImageSize = 0;
    si.imageUserId = -1;
    si.imageMeasurementId = -1;
    si.imageRecipeId = -1;
    si.imagePhotoType.clear();
    si.imageFromAdmin = false;
}

void Server::processCommand(QTcpSocket* currentSocket, const QString& command)
{
    int index = findSocketIndex(currentSocket);
    if(index == -1) return;

    SocketInfo& info = Sockets[index];

    // ✅ Централизованный гейт авторизации:
    // Команды AUTH/REG/ADMIN_LOGIN/LoadUserData не требуют предварительной аутентификации
    // (LoadUserData — это часть восстановления сессии на Android, см. handlers ниже).
    // Все остальные команды требуют info.authenticated == true.
    const bool isPublic =
        command.startsWith("AUTH|") ||
        command.startsWith("REG|") ||
        command.startsWith("ADMIN_LOGIN|") ||
        command.startsWith("LoadUserData|");

    if (!isPublic && !info.authenticated) {
        qWarning() << "🚫 Unauthenticated command rejected:" << command.left(30)
                   << "from IP:" << info.clientIp;
        SendToClient(currentSocket, "UNAUTHORIZED");
        return;
    }

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

            // ✅ Особая логика: на Android сессия восстанавливается без AUTH,
            // поэтому первая LoadUserData "связывает" сокет с userId.
            // После связки команда становится IDOR-safe (требует совпадения).
            if (info.userId == -1) {
                info.userId = user_id;
                info.authenticated = true;     // session restored
            } else if (info.userId != user_id) {
                qWarning() << "🚫 IDOR in LoadUserData: socket user=" << info.userId
                           << "tried user=" << user_id << "from IP:" << info.clientIp;
                SendToClient(currentSocket, "FORBIDDEN");
                return;
            }

            QString first_name;
            QString last_name;
            QString gender;
            QString age;
            double height = 0;
            double weight = 0;
            QString goal = "...";
            int planTrainningUserData = 0;
            int standardSubscription = 0;
            int vipSubscription = 0;
            int subscriptionFrozen = 0;
            int feedbackSubscription = 0;

            if(UserManagers.loadUserData(user_id, first_name, last_name, gender,
                                          age, height, weight, goal, planTrainningUserData,
                                          standardSubscription, vipSubscription, subscriptionFrozen,
                                          feedbackSubscription)) {
                SendToClient(currentSocket,
                             QString("LOADUSERDATA|%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12")
                                 .arg(first_name).arg(last_name).arg(gender).arg(age)
                                 .arg((int)height).arg((int)weight).arg(goal)
                                 .arg(planTrainningUserData).arg(standardSubscription)
                                 .arg(vipSubscription).arg(subscriptionFrozen)
                                 .arg(feedbackSubscription));

                qDebug() << "User data loaded for user ID:" << user_id
                         << "subscription:" << standardSubscription
                         << "vip:" << vipSubscription
                         << "feedback:" << feedbackSubscription
                         << "frozen:" << subscriptionFrozen;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
            bool ok;
            quint64 imgSize = parts.at(2).toULongLong(&ok);

            qDebug() << "IMGAVATAR request: user_id=" << user_id << "size=" << imgSize;

            if(ok && imgSize > 0 && imgSize <= MAX_IMAGE_SIZE) {
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
                    // ✅ Данные придут несколькими пакетами — состояние per-socket
                    info.imageBuffer.clear();
                    info.imageBuffer.append(remainingData);
                    info.expectedImageSize = imgSize;
                    info.imageUserId = user_id;
                    info.imageMeasurementId = -1;
                    info.imagePhotoType.clear();
                    info.isReceivingImage = true;

                    qDebug() << "Image receiving started. Have:" << info.imageBuffer.size()
                             << "Need:" << info.expectedImageSize;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;

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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
            QString date = parts.at(2);
            QString weight = parts.at(3);
            QString chest = parts.at(4);
            QString waist = parts.at(5);
            QString hips = parts.at(6);
            QString steps = parts.at(7);

            int measurement_id = -1;
            if(UserManagers.createMeasurement(user_id, date, weight, chest, waist, hips, steps, measurement_id)) {
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
            int measurement_id = parts.at(2).toInt();
            QString photoType = parts.at(3);
            bool ok;
            quint64 imgSize = parts.at(4).toULongLong(&ok);

            qDebug() << "UPLOAD_MEASUREMENT_PHOTO: user_id=" << user_id << "measurement_id=" << measurement_id
                     << "type=" << photoType << "size=" << imgSize;

            // ✅ Валидация photoType — только конкретные значения
            if (photoType != "front" && photoType != "side" && photoType != "back") {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                qDebug() << "Invalid photoType:" << photoType;
                return;
            }

            if(ok && imgSize > 0 && imgSize <= MAX_IMAGE_SIZE) {
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
                    // ✅ Per-socket state
                    info.imageBuffer.clear();
                    info.imageBuffer.append(remainingData);
                    info.expectedImageSize = imgSize;
                    info.imageUserId = user_id;
                    info.imageMeasurementId = measurement_id;
                    info.imagePhotoType = photoType;
                    info.isReceivingImage = true;

                    qDebug() << "Waiting for rest of image. Have:" << info.imageBuffer.size()
                             << "Need:" << info.expectedImageSize;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;

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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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

    // ADD_DISH_TO_DIET|user_id|date|recipe_name_base64
    // Начисляет призовые баллы за добавление уникального блюда в рацион (один раз за день на блюдо).
    else if (command.startsWith("ADD_DISH_TO_DIET|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 4) {
            int user_id = parts.at(1).toInt();
            QString date = parts.at(2);
            // имя может содержать «|», поэтому передаём base64
            QString nameB64 = parts.mid(3).join("|");
            QString recipeName = QString::fromUtf8(QByteArray::fromBase64(nameB64.toLatin1()));

            int awarded = 0;
            if (UserManagers.addDishToDiet(user_id, recipeName, date, awarded)) {
                int total = 0;
                UserManagers.getUserKachballs(user_id, total);
                SendToClient(currentSocket,
                             QString("DISH_DIET_REWARD|%1|%2").arg(awarded).arg(total));
                qDebug() << "✅ ADD_DISH_TO_DIET user" << user_id
                         << "dish:" << recipeName << "+" << awarded;
            } else {
                SendToClient(currentSocket, "DISH_DIET_REWARD_FAILED");
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
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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

    // ═════════════════════════════════════════════════════════════════════
    // ✅ ЧАТ ПОДДЕРЖКИ (доступен пользователям с Feedback или VIP подпиской)
    // ═════════════════════════════════════════════════════════════════════
    // CHAT_SEND|user_id|<base64-text>
    else if (command.startsWith("CHAT_SEND|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "CHAT_SEND")) return;
            QString text = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            qint64 msgId = 0;
            if (UserManagers.sendChatMessage(user_id, /*fromAdmin=*/false, text, msgId))
                SendToClient(currentSocket, QString("CHAT_SENT|%1").arg(msgId));
            else
                SendToClient(currentSocket, "CHAT_SEND_FAILED");
        }
    }
    // CHAT_SEND_PHOTO|user_id|size + bytes — пользователь шлёт фото в чат
    else if (command.startsWith("CHAT_SEND_PHOTO|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "CHAT_SEND_PHOTO")) return;
            bool ok = false;
            quint64 imgSize = parts.at(2).toULongLong(&ok);

            if (!ok || imgSize == 0 || imgSize > MAX_IMAGE_SIZE) {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                return;
            }

            QByteArray remainingData = currentSocket->readAll();
            if (remainingData.size() >= (qint64)imgSize) {
                QByteArray imageData = remainingData.left(imgSize);
                qint64 msgId = 0;
                if (UserManagers.sendChatPhoto(user_id, /*fromAdmin=*/false, imageData, msgId)) {
                    SendToClient(currentSocket, QString("CHAT_PHOTO_SAVED|%1").arg(msgId));
                } else {
                    SendToClient(currentSocket, "CHAT_PHOTO_SAVE_FAILED");
                }
            } else {
                // Пакет придёт частями — настраиваем per-socket state
                info.imageBuffer.clear();
                info.imageBuffer.append(remainingData);
                info.expectedImageSize = imgSize;
                info.imageUserId = user_id;
                info.imageMeasurementId = -1;
                info.imageRecipeId = -1;
                info.imagePhotoType = "chat";
                info.imageFromAdmin = false;
                info.isReceivingImage = true;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }
    // GET_CHAT_PHOTO|message_id — загрузить бинарь конкретного фото-сообщения
    else if (command.startsWith("GET_CHAT_PHOTO|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            qint64 msgId = parts.at(1).toLongLong();
            QByteArray photoData;
            if (UserManagers.getChatPhoto(msgId, photoData)) {
                // Заголовок: GET_CHAT_PHOTO|msgId|size, далее бинарные байты
                SendToClient(currentSocket,
                             QString("GET_CHAT_PHOTO|%1|%2").arg(msgId).arg(photoData.size()));
                currentSocket->write(photoData);
            } else {
                SendToClient(currentSocket, QString("CHAT_PHOTO_NOT_FOUND|%1").arg(msgId));
            }
        }
    }
    // ═════════════════════════════════════════════════════════════════════
    // ✅ ЧАТ ТЕХНИЧЕСКОЙ ПОДДЕРЖКИ (доступен ВСЕМ юзерам, не зависит от тарифа)
    // ═════════════════════════════════════════════════════════════════════
    // SUPPORT_SEND|user_id|<base64-text>
    else if (command.startsWith("SUPPORT_SEND|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "SUPPORT_SEND")) return;
            QString text = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            qint64 msgId = 0;
            if (UserManagers.sendSupportMessage(user_id, /*fromAdmin=*/false, text, msgId))
                SendToClient(currentSocket, QString("SUPPORT_SENT|%1").arg(msgId));
            else
                SendToClient(currentSocket, "SUPPORT_SEND_FAILED");
        }
    }
    // SUPPORT_LOAD|user_id
    else if (command.startsWith("SUPPORT_LOAD|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "SUPPORT_LOAD")) return;
            QString json;
            if (UserManagers.loadSupportHistory(user_id, json)) {
                UserManagers.markSupportRead(user_id, /*byAdmin=*/false);
                SendToClient(currentSocket, "SUPPORT_HISTORY|" + json);
            } else {
                SendToClient(currentSocket, "SUPPORT_LOAD_FAILED");
            }
        }
    }
    // SUPPORT_UNREAD|user_id
    else if (command.startsWith("SUPPORT_UNREAD|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "SUPPORT_UNREAD")) return;
            int n = 0;
            UserManagers.countSupportUnreadForUser(user_id, n);
            SendToClient(currentSocket, QString("SUPPORT_UNREAD_COUNT|%1").arg(n));
        }
    }
    // SUPPORT_CLEAR|user_id
    else if (command.startsWith("SUPPORT_CLEAR|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "SUPPORT_CLEAR")) return;
            int deleted = 0;
            if (UserManagers.clearSupportHistory(user_id, deleted))
                SendToClient(currentSocket, QString("SUPPORT_CLEARED|%1").arg(deleted));
            else
                SendToClient(currentSocket, "SUPPORT_CLEAR_FAILED");
        }
    }
    // SUPPORT_SEND_PHOTO|user_id|size + bytes
    else if (command.startsWith("SUPPORT_SEND_PHOTO|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "SUPPORT_SEND_PHOTO")) return;
            bool ok = false;
            quint64 imgSize = parts.at(2).toULongLong(&ok);
            if (!ok || imgSize == 0 || imgSize > MAX_IMAGE_SIZE) {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                return;
            }
            QByteArray remainingData = currentSocket->readAll();
            if (remainingData.size() >= (qint64)imgSize) {
                QByteArray imgData = remainingData.left(imgSize);
                qint64 msgId = 0;
                if (UserManagers.sendSupportPhoto(user_id, /*fromAdmin=*/false, imgData, msgId))
                    SendToClient(currentSocket, QString("SUPPORT_PHOTO_SAVED|%1").arg(msgId));
                else
                    SendToClient(currentSocket, "SUPPORT_PHOTO_SAVE_FAILED");
            } else {
                info.imageBuffer.clear();
                info.imageBuffer.append(remainingData);
                info.expectedImageSize = imgSize;
                info.imageUserId = user_id;
                info.imageMeasurementId = -1;
                info.imageRecipeId = -1;
                info.imagePhotoType = "support";
                info.imageFromAdmin = false;
                info.isReceivingImage = true;
            }
        }
    }
    // GET_SUPPORT_PHOTO|message_id
    else if (command.startsWith("GET_SUPPORT_PHOTO|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            qint64 msgId = parts.at(1).toLongLong();
            QByteArray photoData;
            if (UserManagers.getSupportPhoto(msgId, photoData)) {
                SendToClient(currentSocket,
                             QString("GET_SUPPORT_PHOTO|%1|%2").arg(msgId).arg(photoData.size()));
                currentSocket->write(photoData);
            } else {
                SendToClient(currentSocket, QString("SUPPORT_PHOTO_NOT_FOUND|%1").arg(msgId));
            }
        }
    }
    // ─── Админ: техподдержка ──────────────────────────────────
    else if (command == "ADMIN_GET_SUPPORT_CONVERSATIONS") {
        QString json;
        if (UserManagers.getAdminSupportConversations(json))
            SendToClient(currentSocket, "ADMIN_SUPPORT_CONVERSATIONS_DATA|" + json);
        else
            SendToClient(currentSocket, "ADMIN_SUPPORT_CONVERSATIONS_FAILED");
    }
    else if (command.startsWith("ADMIN_GET_SUPPORT_CHAT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString json;
            if (UserManagers.loadSupportHistory(user_id, json)) {
                UserManagers.markSupportRead(user_id, /*byAdmin=*/true);
                SendToClient(currentSocket, QString("ADMIN_SUPPORT_CHAT_DATA|%1|%2").arg(user_id).arg(json));
            } else {
                SendToClient(currentSocket, "ADMIN_SUPPORT_CHAT_FAILED");
            }
        }
    }
    else if (command.startsWith("ADMIN_SEND_SUPPORT_MESSAGE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            QString text = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            qint64 msgId = 0;
            if (UserManagers.sendSupportMessage(user_id, /*fromAdmin=*/true, text, msgId)) {
                SendToClient(currentSocket,
                             QString("ADMIN_SUPPORT_MESSAGE_SENT|%1|%2").arg(user_id).arg(msgId));
                if (QTcpSocket* userSock = findSocketByUserId(user_id))
                    SendToClient(userSock, "SUPPORT_NEW_MESSAGE");
            } else {
                SendToClient(currentSocket, "ADMIN_SUPPORT_MESSAGE_FAILED");
            }
        }
    }
    else if (command.startsWith("ADMIN_SEND_SUPPORT_PHOTO|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            bool ok = false;
            quint64 imgSize = parts.at(2).toULongLong(&ok);
            if (!ok || imgSize == 0 || imgSize > MAX_IMAGE_SIZE) {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                return;
            }
            QByteArray remainingData = currentSocket->readAll();
            if (remainingData.size() >= (qint64)imgSize) {
                QByteArray imgData = remainingData.left(imgSize);
                qint64 msgId = 0;
                if (UserManagers.sendSupportPhoto(user_id, /*fromAdmin=*/true, imgData, msgId)) {
                    SendToClient(currentSocket,
                                 QString("ADMIN_SUPPORT_PHOTO_SENT|%1|%2").arg(user_id).arg(msgId));
                    if (QTcpSocket* userSock = findSocketByUserId(user_id))
                        SendToClient(userSock, "SUPPORT_NEW_MESSAGE");
                } else {
                    SendToClient(currentSocket, "ADMIN_SUPPORT_PHOTO_FAILED");
                }
            } else {
                info.imageBuffer.clear();
                info.imageBuffer.append(remainingData);
                info.expectedImageSize = imgSize;
                info.imageUserId = user_id;
                info.imageMeasurementId = -1;
                info.imageRecipeId = -1;
                info.imagePhotoType = "support";
                info.imageFromAdmin = true;
                info.isReceivingImage = true;
            }
        }
    }
    else if (command.startsWith("ADMIN_CLEAR_SUPPORT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            int deleted = 0;
            if (UserManagers.clearSupportHistory(user_id, deleted))
                SendToClient(currentSocket,
                             QString("ADMIN_SUPPORT_CLEARED|%1|%2").arg(user_id).arg(deleted));
            else
                SendToClient(currentSocket, "ADMIN_SUPPORT_CLEAR_FAILED");
        }
    }
    // CHAT_CLEAR|user_id — пользователь стирает свою переписку с тренером
    else if (command.startsWith("CHAT_CLEAR|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "CHAT_CLEAR")) return;
            int deleted = 0;
            if (UserManagers.clearChatHistory(user_id, deleted)) {
                SendToClient(currentSocket, QString("CHAT_CLEARED|%1").arg(deleted));
                // Уведомим админку (если кто-то её смотрит) — переписки больше нет
                // через простое отображение «нет диалогов» при следующем GET_CONVERSATIONS.
            } else {
                SendToClient(currentSocket, "CHAT_CLEAR_FAILED");
            }
        }
    }
    // CHAT_LOAD|user_id — загрузить историю
    else if (command.startsWith("CHAT_LOAD|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "CHAT_LOAD")) return;
            QString json;
            if (UserManagers.loadChatHistory(user_id, json)) {
                UserManagers.markChatRead(user_id, /*byAdmin=*/false);
                SendToClient(currentSocket, "CHAT_HISTORY|" + json);
            } else {
                SendToClient(currentSocket, "CHAT_LOAD_FAILED");
            }
        }
    }
    // CHAT_UNREAD|user_id
    else if (command.startsWith("CHAT_UNREAD|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "CHAT_UNREAD")) return;
            int n = 0;
            UserManagers.countUnreadForUser(user_id, n);
            SendToClient(currentSocket, QString("CHAT_UNREAD_COUNT|%1").arg(n));
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // ✅ УРОКИ — отметка просмотра + начисление баллов
    // ═════════════════════════════════════════════════════════════════════
    else if (command.startsWith("MARK_LESSON_VIEWED|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "MARK_LESSON_VIEWED")) return;
            QString key = parts.at(2);
            int awarded = 0;
            if (UserManagers.markLessonViewed(user_id, key, awarded))
                SendToClient(currentSocket, QString("LESSON_VIEWED_OK|%1|%2").arg(key).arg(awarded));
            else
                SendToClient(currentSocket, "LESSON_VIEWED_FAILED");
        }
    }
    else if (command.startsWith("GET_VIEWED_LESSONS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "GET_VIEWED_LESSONS")) return;
            QString json;
            if (UserManagers.loadViewedLessons(user_id, json))
                SendToClient(currentSocket, "VIEWED_LESSONS|" + json);
            else
                SendToClient(currentSocket, "VIEWED_LESSONS_FAILED");
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // ✅ ИЗБРАННЫЕ РЕЦЕПТЫ
    // ═════════════════════════════════════════════════════════════════════
    else if (command.startsWith("ADD_FAVORITE_RECIPE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "ADD_FAVORITE_RECIPE")) return;
            int recipe_id = parts.at(2).toInt();
            if (UserManagers.addFavoriteRecipe(user_id, recipe_id))
                SendToClient(currentSocket, QString("FAVORITE_ADDED|%1").arg(recipe_id));
            else
                SendToClient(currentSocket, "FAVORITE_ADD_FAILED");
        }
    }
    else if (command.startsWith("REMOVE_FAVORITE_RECIPE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "REMOVE_FAVORITE_RECIPE")) return;
            int recipe_id = parts.at(2).toInt();
            if (UserManagers.removeFavoriteRecipe(user_id, recipe_id))
                SendToClient(currentSocket, QString("FAVORITE_REMOVED|%1").arg(recipe_id));
            else
                SendToClient(currentSocket, "FAVORITE_REMOVE_FAILED");
        }
    }
    else if (command.startsWith("GET_FAVORITE_RECIPES|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "GET_FAVORITE_RECIPES")) return;
            QString json;
            if (UserManagers.loadFavoriteRecipes(user_id, json))
                SendToClient(currentSocket, "FAVORITE_RECIPES|" + json);
            else
                SendToClient(currentSocket, "FAVORITE_RECIPES_FAILED");
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // ✅ PUSH-устройства
    // ═════════════════════════════════════════════════════════════════════
    else if (command.startsWith("REGISTER_DEVICE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 4) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "REGISTER_DEVICE")) return;
            QString token = parts.at(2);
            QString platform = parts.at(3);
            if (UserManagers.registerDevice(user_id, token, platform))
                SendToClient(currentSocket, "DEVICE_REGISTERED");
            else
                SendToClient(currentSocket, "DEVICE_REGISTER_FAILED");
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // ✅ COOKED_DISH — фото приготовленного блюда → +5 баллов
    // Формат: COOKED_DISH|user_id|recipe_id|imgSize  + бинарные байты в потоке
    // ═════════════════════════════════════════════════════════════════════
    else if (command.startsWith("COOKED_DISH|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 4) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, "COOKED_DISH")) return;
            int recipe_id = parts.at(2).toInt();
            bool ok = false;
            quint64 imgSize = parts.at(3).toULongLong(&ok);
            if (!ok || imgSize == 0 || imgSize > MAX_IMAGE_SIZE) {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                return;
            }
            QByteArray remainingData = currentSocket->readAll();
            if ((quint64)remainingData.size() >= imgSize) {
                QByteArray photo = remainingData.left(imgSize);
                int dishId = 0;
                if (UserManagers.saveCookedDish(user_id, recipe_id, photo, dishId))
                    SendToClient(currentSocket, QString("DISH_SAVED|%1|5").arg(dishId));   // +5 баллов
                else
                    SendToClient(currentSocket, "DISH_SAVE_FAILED");
            } else {
                // данные придут частями
                info.imageBuffer.clear();
                info.imageBuffer.append(remainingData);
                info.expectedImageSize = imgSize;
                info.imageUserId = user_id;
                info.imageRecipeId = recipe_id;
                info.imageMeasurementId = -1;
                info.imagePhotoType = "dish";
                info.isReceivingImage = true;
            }
        }
    }

    // ✅ SAVE_PROGRAM|user_id|planInt
    // Сохраняет выбранный план тренировок в колонку PlanTrainning таблицы USERS
    // planInt: 0 = не выбрано, 1 = gym_1 (добавляйте новые значения при расширении)
    else if (command.startsWith("SAVE_PROGRAM|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            if (!requireOwnership(currentSocket, info, user_id, command.left(40))) return;
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

    // ✅ ADMIN_LOGIN|password — аутентификация админа
    // Сравниваем SHA-256 хеш введённого пароля с тем, что сохранён в server_config.ini
    // (рядом с exe). При первом запуске создаётся файл с дефолтным паролем Kech_Admin_2024.
    else if (command.startsWith("ADMIN_LOGIN|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            // ✅ Rate limit на админ-логин — защита от bruteforce.
            // Подсчитываем неудачные попытки на IP за последнюю минуту.
            qint64 nowMs2 = QDateTime::currentMSecsSinceEpoch();
            QVector<qint64>& fails = adminLoginFailures[info.clientIp];
            while (!fails.isEmpty() && fails.first() < nowMs2 - 60000)
                fails.removeFirst();
            if (fails.size() >= 5) {
                qWarning() << "🚫 Admin login bruteforce blocked from:" << info.clientIp;
                SendToClient(currentSocket, "ADMIN_LOGIN_FAILED");
                return;
            }

            // Загружаем конфиг (или создаём с дефолтным паролем)
            const QString cfgPath = QCoreApplication::applicationDirPath() + "/server_config.ini";
            QSettings cfg(cfgPath, QSettings::IniFormat);
            QString storedHash = cfg.value("admin/password_sha256").toString();
            if (storedHash.isEmpty()) {
                // Первый запуск: ставим дефолтный пароль
                const QByteArray defHash = QCryptographicHash::hash(
                    QByteArray("Kech_Admin_2024"), QCryptographicHash::Sha256).toHex();
                storedHash = QString::fromLatin1(defHash);
                cfg.setValue("admin/password_sha256", storedHash);
                cfg.sync();
                qWarning() << "⚠️ Создан server_config.ini с дефолтным админ-паролем."
                           << " СМЕНИТЕ его в файле:" << cfgPath;
            }

            const QByteArray inputHash = QCryptographicHash::hash(
                parts.at(1).toUtf8(), QCryptographicHash::Sha256).toHex();

            if (QString::fromLatin1(inputHash).compare(storedHash, Qt::CaseInsensitive) == 0) {
                info.adminAuthenticated = true;
                info.authenticated = true;
                fails.clear();
                SendToClient(currentSocket, "ADMIN_LOGIN_OK");
                qDebug() << "🔐 Admin authenticated from IP:" << info.clientIp;
            } else {
                fails.append(nowMs2);
                SendToClient(currentSocket, "ADMIN_LOGIN_FAILED");
                qWarning() << "🚫 Failed admin login from IP:" << info.clientIp
                           << "(attempts:" << fails.size() << ")";
            }
        }
    }
    // ✅ Защита: все остальные ADMIN_ команды требуют admin-аутентификации
    else if (command.startsWith("ADMIN_") && !info.adminAuthenticated) {
        qWarning() << "🚫 Unauthorized admin command attempt:" << command.left(50)
                   << "from IP:" << info.clientIp;
        SendToClient(currentSocket, "ADMIN_UNAUTHORIZED");
        return;
    }

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

    // ✅ ADMIN_SET_FROZEN|user_id|0or1[|days]
    //   frozen=1: лимит 2/год, по умолчанию 14 дней, авто-разморозка по таймеру.
    //   frozen=0: ручная разморозка.
    else if (command.startsWith("ADMIN_SET_FROZEN|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            int frozen  = parts.at(2).toInt() ? 1 : 0;
            int days    = parts.size() >= 4 ? parts.at(3).toInt() : 14;

            bool ok = false;
            QString errMsg;
            if (frozen) {
                ok = UserManagers.freezeSubscription(user_id, days, errMsg);
            } else {
                ok = UserManagers.unfreezeSubscription(user_id);
            }
            if (ok) {
                SendToClient(currentSocket, QString("ADMIN_FROZEN_OK|%1|%2").arg(user_id).arg(frozen));
                if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                    SendToClient(userSock, QString("SUBSCRIPTION_FROZEN_CHANGED|%1").arg(frozen));
                }
            } else {
                SendToClient(currentSocket, "ADMIN_FROZEN_FAILED|" + errMsg);
            }
        }
    }
    // ✅ ADMIN_BILLING_SAVE_METHOD|user_id|payment_method_id|payment_method_title|tier
    //    Вызывается сайтом после первой успешной оплаты (webhook payment.succeeded).
    //    Сохраняет сохранённую карту YooKassa и взводит next_billing_date = today+30д.
    //    tier ∈ {'lite','feedback'} — VIP не авто-продлевается.
    else if (command.startsWith("ADMIN_BILLING_SAVE_METHOD|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 5) {
            int user_id            = parts.at(1).toInt();
            QString paymentMethod  = parts.at(2);
            QString methodTitle    = parts.at(3);
            QString tier           = parts.at(4);
            if (UserManagers.billingSaveMethod(user_id, paymentMethod, methodTitle, tier)) {
                SendToClient(currentSocket,
                             QString("ADMIN_BILLING_SAVE_OK|%1|%2").arg(user_id).arg(tier));
                qDebug() << "ADMIN_BILLING_SAVE_METHOD: user=" << user_id << "tier=" << tier;
            } else {
                SendToClient(currentSocket, "ADMIN_BILLING_SAVE_FAILED");
            }
        } else {
            SendToClient(currentSocket, "ADMIN_BILLING_SAVE_FAILED|BAD_FORMAT");
        }
    }
    // ✅ ADMIN_BILLING_DUE — список пользователей, готовых к авто-списанию.
    //    Возвращает JSON-массив [{user_id, tier, payment_method_id, retry_count}, ...]
    //    Сайт-cron берёт его раз в день и для каждого делает YooKassa-платёж.
    else if (command == "ADMIN_BILLING_DUE") {
        QList<UserManager::BillingDueRow> rows;
        if (UserManagers.billingGetDue(rows)) {
            QJsonArray arr;
            for (const auto &r : rows) {
                QJsonObject o;
                o["user_id"]           = r.userId;
                o["tier"]              = r.tier;
                o["payment_method_id"] = r.paymentMethodId;
                o["retry_count"]       = r.retryCount;
                o["email"]             = r.email;
                o["first_name"]        = r.firstName;
                arr.append(o);
            }
            QString json = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
            SendToClient(currentSocket, "ADMIN_BILLING_DUE_DATA|" + json);
        } else {
            SendToClient(currentSocket, "ADMIN_BILLING_DUE_FAILED");
        }
    }
    // ✅ ADMIN_BILLING_SUCCESS|user_id — пришла успешная YooKassa-транзакция.
    //    Продлеваем подписку на 30 дней, сбрасываем retry, при необходимости размораживаем.
    else if (command.startsWith("ADMIN_BILLING_SUCCESS|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            // Узнаём, была ли подписка заморожена по биллингу — для уведомления клиента.
            int wasFrozen = 0;
            UserManagers.getSubscriptionFrozen(user_id, wasFrozen);

            if (UserManagers.billingRecordSuccess(user_id)) {
                SendToClient(currentSocket,
                             QString("ADMIN_BILLING_SUCCESS_OK|%1").arg(user_id));
                // Если юзер онлайн и размораживается — пушим обновление флага
                if (wasFrozen) {
                    if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                        SendToClient(userSock, "SUBSCRIPTION_FROZEN_CHANGED|0");
                    }
                }
            } else {
                SendToClient(currentSocket, "ADMIN_BILLING_SUCCESS_FAILED");
            }
        }
    }
    // ✅ ADMIN_BILLING_FAILED|user_id — не удалось списать с карты.
    //    Увеличиваем retry; на 3-й подряд неудаче — заморозка (freeze_reason='billing').
    else if (command.startsWith("ADMIN_BILLING_FAILED|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            bool nowFrozen = false;
            if (UserManagers.billingRecordFailure(user_id, nowFrozen)) {
                SendToClient(currentSocket,
                             QString("ADMIN_BILLING_FAILED_OK|%1|%2")
                                 .arg(user_id).arg(nowFrozen ? 1 : 0));
                // Если только что заморозили — уведомим онлайн-клиент,
                // чтобы NoSubscriptionScreen появился без перезахода в приложение.
                if (nowFrozen) {
                    if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                        SendToClient(userSock, "SUBSCRIPTION_FROZEN_CHANGED|1");
                    }
                }
            } else {
                SendToClient(currentSocket, "ADMIN_BILLING_FAILED_FAILED");
            }
        }
    }
    // ✅ ADMIN_BILLING_CANCEL|user_id — пользователь отменил авто-продление в ЛК.
    //    Сохранённая карта удаляется из биллинга (флаг auto_renew=0). Текущий тариф остаётся
    //    активным до конца оплаченного периода (его сбросит админ или истёкший срок отдельно).
    else if (command.startsWith("ADMIN_BILLING_CANCEL|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (UserManagers.billingCancelAutoRenew(user_id)) {
                SendToClient(currentSocket,
                             QString("ADMIN_BILLING_CANCEL_OK|%1").arg(user_id));
            } else {
                SendToClient(currentSocket, "ADMIN_BILLING_CANCEL_FAILED");
            }
        }
    }
    // ✅ ADMIN_BILLING_GET|user_id — текущее состояние биллинга для страницы /account.
    else if (command.startsWith("ADMIN_BILLING_GET|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString json;
            if (UserManagers.billingGetState(user_id, json)) {
                SendToClient(currentSocket, "ADMIN_BILLING_STATE|" + json);
            } else {
                SendToClient(currentSocket, "ADMIN_BILLING_GET_FAILED");
            }
        }
    }
    // ✅ ADMIN_SET_FEEDBACK|user_id|0or1 — активация/снятие подписки "с обратной связью"
    else if (command.startsWith("ADMIN_SET_FEEDBACK|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            int status  = parts.at(2).toInt() ? 1 : 0;
            if (UserManagers.setFeedbackSubscription(user_id, status)) {
                SendToClient(currentSocket, QString("ADMIN_FEEDBACK_OK|%1|%2").arg(user_id).arg(status));
                if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                    SendToClient(userSock, QString("FEEDBACK_SUBSCRIPTION_CHANGED|%1").arg(status));
                }
            } else {
                SendToClient(currentSocket, "ADMIN_FEEDBACK_FAILED");
            }
        }
    }
    // ✅ ADMIN_GET_CONVERSATIONS — список юзеров, у которых есть чат + непрочитанные
    else if (command == "ADMIN_GET_CONVERSATIONS") {
        QString jsonData;
        if (UserManagers.getAdminConversations(jsonData))
            SendToClient(currentSocket, "ADMIN_CONVERSATIONS_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_CONVERSATIONS_FAILED");
    }
    // ✅ ADMIN_GET_CHAT|user_id — история переписки с конкретным юзером
    else if (command.startsWith("ADMIN_GET_CHAT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString json;
            if (UserManagers.loadChatHistory(user_id, json)) {
                UserManagers.markChatRead(user_id, /*byAdmin=*/true);
                SendToClient(currentSocket, QString("ADMIN_CHAT_DATA|%1|%2").arg(user_id).arg(json));
            } else {
                SendToClient(currentSocket, "ADMIN_CHAT_FAILED");
            }
        }
    }
    // ✅ ADMIN_SEND_MESSAGE|user_id|<base64-text>
    else if (command.startsWith("ADMIN_SEND_MESSAGE|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            QString text = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            qint64 msgId = 0;
            if (UserManagers.sendChatMessage(user_id, /*fromAdmin=*/true, text, msgId)) {
                SendToClient(currentSocket, QString("ADMIN_MESSAGE_SENT|%1|%2").arg(user_id).arg(msgId));
                // онлайн-получатель — пушим ему новое сообщение
                if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                    SendToClient(userSock, "CHAT_NEW_MESSAGE");
                }
            } else {
                SendToClient(currentSocket, "ADMIN_MESSAGE_FAILED");
            }
        }
    }
    // ✅ ADMIN_CLEAR_CHAT|user_id — админ «закрывает диалог», стирая переписку
    else if (command.startsWith("ADMIN_CLEAR_CHAT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            int deleted = 0;
            if (UserManagers.clearChatHistory(user_id, deleted)) {
                SendToClient(currentSocket,
                             QString("ADMIN_CHAT_CLEARED|%1|%2").arg(user_id).arg(deleted));
                // Если пользователь онлайн — уведомим, чтобы он перезагрузил чат.
                if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                    SendToClient(userSock, "CHAT_NEW_MESSAGE");
                }
            } else {
                SendToClient(currentSocket, "ADMIN_CHAT_CLEAR_FAILED");
            }
        }
    }
    // ✅ ADMIN_SEND_CHAT_PHOTO|user_id|size + bytes — админ шлёт фото в чат
    else if (command.startsWith("ADMIN_SEND_CHAT_PHOTO|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            bool ok = false;
            quint64 imgSize = parts.at(2).toULongLong(&ok);

            if (!ok || imgSize == 0 || imgSize > MAX_IMAGE_SIZE) {
                SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
                return;
            }

            QByteArray remainingData = currentSocket->readAll();
            if (remainingData.size() >= (qint64)imgSize) {
                QByteArray imageData = remainingData.left(imgSize);
                qint64 msgId = 0;
                if (UserManagers.sendChatPhoto(user_id, /*fromAdmin=*/true, imageData, msgId)) {
                    SendToClient(currentSocket,
                                 QString("ADMIN_CHAT_PHOTO_SENT|%1|%2").arg(user_id).arg(msgId));
                    if (QTcpSocket* userSock = findSocketByUserId(user_id)) {
                        SendToClient(userSock, "CHAT_NEW_MESSAGE");
                    }
                } else {
                    SendToClient(currentSocket, "ADMIN_CHAT_PHOTO_FAILED");
                }
            } else {
                // Фото придёт частями
                info.imageBuffer.clear();
                info.imageBuffer.append(remainingData);
                info.expectedImageSize = imgSize;
                info.imageUserId = user_id;
                info.imageMeasurementId = -1;
                info.imageRecipeId = -1;
                info.imagePhotoType = "chat";
                info.imageFromAdmin = true;
                info.isReceivingImage = true;
            }
        } else {
            SendToClient(currentSocket, "BAD_COMMAND_FORMAT");
        }
    }
    // ✅ ADMIN_LIST_DEVICES|user_id — список push-устройств юзера
    else if (command.startsWith("ADMIN_LIST_DEVICES|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString json;
            if (UserManagers.loadUserDevices(user_id, json))
                SendToClient(currentSocket, "ADMIN_DEVICES_DATA|" + json);
            else
                SendToClient(currentSocket, "ADMIN_DEVICES_FAILED");
        }
    }
    // ✅ ADMIN_PUSH_LIST_DEVICES — все устройства всех пользователей.
    //    Используется сайтом-cron'ом для массовой рассылки push-уведомлений (ТЗ: 1-3 в день).
    else if (command == "ADMIN_PUSH_LIST_DEVICES") {
        QString json;
        if (UserManagers.loadAllDevices(json))
            SendToClient(currentSocket, "ADMIN_PUSH_DEVICES_DATA|" + json);
        else
            SendToClient(currentSocket, "ADMIN_PUSH_DEVICES_FAILED");
    }
    // ✅ ADMIN_PUSH_UNREGISTER|token — снять регистрацию устройства, когда FCM
    //    вернул UNREGISTERED / NOT_FOUND (токен протух, переустановили приложение).
    else if (command.startsWith("ADMIN_PUSH_UNREGISTER|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            QString token = parts.at(1);
            if (UserManagers.removeDeviceByToken(token))
                SendToClient(currentSocket, "ADMIN_PUSH_UNREGISTER_OK");
            else
                SendToClient(currentSocket, "ADMIN_PUSH_UNREGISTER_FAILED");
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

    // ADMIN_GET_ACHIEVEMENTS — загрузить все достижения с наградами
    else if (command == "ADMIN_GET_ACHIEVEMENTS") {
        QString jsonData;
        if (UserManagers.getAllAchievements(jsonData))
            SendToClient(currentSocket, "ADMIN_ACHIEVEMENTS_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_ACHIEVEMENTS_FAILED");
    }

    // ADMIN_UPDATE_REWARD|achievement_id|new_reward
    else if (command.startsWith("ADMIN_UPDATE_REWARD|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int achId = parts.at(1).toInt();
            int newReward = parts.at(2).toInt();
            if (UserManagers.updateAchievementReward(achId, newReward)) {
                QString jsonData;
                UserManagers.getAllAchievements(jsonData);
                SendToClient(currentSocket, "ADMIN_ACHIEVEMENTS_DATA|" + jsonData);
                qDebug() << "ADMIN_UPDATE_REWARD OK: id=" << achId << "reward=" << newReward;
            } else {
                SendToClient(currentSocket, "ADMIN_ACHIEVEMENTS_FAILED");
            }
        }
    }

    // ADMIN_GET_BONUS_SETTINGS — список настраиваемых призовых баллов
    else if (command == "ADMIN_GET_BONUS_SETTINGS") {
        QString jsonData;
        if (UserManagers.getAllBonusSettings(jsonData))
            SendToClient(currentSocket, "ADMIN_BONUS_SETTINGS_DATA|" + jsonData);
        else
            SendToClient(currentSocket, "ADMIN_BONUS_SETTINGS_FAILED");
    }

    // ADMIN_SET_BONUS_SETTING|key|amount
    else if (command.startsWith("ADMIN_SET_BONUS_SETTING|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            QString key = parts.at(1);
            int amount = parts.at(2).toInt();
            if (UserManagers.setBonusAmount(key, amount)) {
                QString jsonData;
                UserManagers.getAllBonusSettings(jsonData);
                SendToClient(currentSocket, "ADMIN_BONUS_SETTINGS_DATA|" + jsonData);
                qDebug() << "ADMIN_SET_BONUS_SETTING OK:" << key << "=" << amount;
            } else {
                SendToClient(currentSocket, "ADMIN_BONUS_SETTINGS_FAILED");
            }
        }
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

    // ==================== VIP ПЕРСОНАЛЬНЫЕ ПРОГРАММЫ ====================

    // ADMIN_SET_VIP|user_id|0or1
    else if (command.startsWith("ADMIN_SET_VIP|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id = parts.at(1).toInt();
            int status  = parts.at(2).toInt();
            if (UserManagers.setVipSubscription(user_id, status)) {
                SendToClient(currentSocket, QString("ADMIN_VIP_OK|%1|%2").arg(user_id).arg(status));
                qDebug() << "ADMIN_SET_VIP: user=" << user_id << "status=" << status;
            } else {
                SendToClient(currentSocket, "ADMIN_VIP_FAILED");
            }
        }
    }

    // ADMIN_CREATE_VIP_PROGRAM|user_id|program_name_b64|workout1_b64|workout2_b64|workout3_b64
    else if (command.startsWith("ADMIN_CREATE_VIP_PROGRAM|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 6) {
            int user_id       = parts.at(1).toInt();
            QString name      = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            QString w1        = QString::fromUtf8(QByteArray::fromBase64(parts.at(3).toLatin1()));
            QString w2        = QString::fromUtf8(QByteArray::fromBase64(parts.at(4).toLatin1()));
            QString w3        = QString::fromUtf8(QByteArray::fromBase64(parts.at(5).toLatin1()));

            if (UserManagers.createVipProgram(user_id, name, w1, w2, w3)) {
                SendToClient(currentSocket, QString("ADMIN_VIP_PROGRAM_CREATED|%1").arg(user_id));
                qDebug() << "ADMIN_CREATE_VIP_PROGRAM: user=" << user_id << "name=" << name;
            } else {
                SendToClient(currentSocket, "ADMIN_VIP_PROGRAM_CREATE_FAILED");
            }
        }
    }

    // ADMIN_LOAD_VIP_PROGRAM|user_id
    else if (command.startsWith("ADMIN_LOAD_VIP_PROGRAM|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.loadVipProgram(user_id, jsonData)) {
                SendToClient(currentSocket, "ADMIN_VIP_PROGRAM_DATA|" + jsonData);
            } else {
                SendToClient(currentSocket, "ADMIN_VIP_PROGRAM_NONE");
            }
        }
    }

    // ADMIN_DELETE_VIP_PROGRAM|user_id
    else if (command.startsWith("ADMIN_DELETE_VIP_PROGRAM|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            if (UserManagers.deleteVipProgram(user_id)) {
                SendToClient(currentSocket, QString("ADMIN_VIP_PROGRAM_DELETED|%1").arg(user_id));
            } else {
                SendToClient(currentSocket, "ADMIN_VIP_PROGRAM_DELETE_FAILED");
            }
        }
    }

    // LOAD_VIP_PROGRAM|user_id — клиент загружает свою VIP-программу
    else if (command.startsWith("LOAD_VIP_PROGRAM|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 2) {
            int user_id = parts.at(1).toInt();
            QString jsonData;
            if (UserManagers.loadVipProgram(user_id, jsonData)) {
                SendToClient(currentSocket, "VIP_PROGRAM_DATA|" + jsonData);
                qDebug() << "LOAD_VIP_PROGRAM: OK for user" << user_id;
            } else {
                SendToClient(currentSocket, "VIP_PROGRAM_NONE");
            }
        }
    }

    // SAVE_VIP_WEIGHT|user_id|exercise_name_b64|a1|a2|a3
    else if (command.startsWith("SAVE_VIP_WEIGHT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 6) {
            int user_id    = parts.at(1).toInt();
            QString exName = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            float a1       = parts.at(3).toFloat();
            float a2       = parts.at(4).toFloat();
            float a3       = parts.at(5).toFloat();

            if (UserManagers.saveVipExerciseWeight(user_id, exName, a1, a2, a3)) {
                SendToClient(currentSocket, "SAVE_VIP_WEIGHT_OK");
            } else {
                SendToClient(currentSocket, "SAVE_VIP_WEIGHT_FAILED");
            }
        }
    }

    // LOAD_VIP_WEIGHT|user_id|exercise_name_b64
    else if (command.startsWith("LOAD_VIP_WEIGHT|")) {
        QStringList parts = command.split("|");
        if (parts.size() >= 3) {
            int user_id    = parts.at(1).toInt();
            QString exName = QString::fromUtf8(QByteArray::fromBase64(parts.at(2).toLatin1()));
            float a1 = 0, a2 = 0, a3 = 0;
            UserManagers.loadVipExerciseWeight(user_id, exName, a1, a2, a3);
            SendToClient(currentSocket, QString("LOAD_VIP_WEIGHT_OK|%1|%2|%3").arg(a1).arg(a2).arg(a3));
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

        // ── Логика 60-дневного предложения смены плана и 90-дневного сброса ──
        bool planChangeDue   = false;
        bool planResetDue    = false;
        bool vipUpdateNeeded = false;
        UserManagers.isPlanChangeDue(user_id, planChangeDue);
        UserManagers.isPlanResetDue(user_id, planResetDue);

        // Получаем VIP-статус — у VIP другая логика обновления планов
        // (программу составляет тренер вручную через админку).
        int vipSub = 0;
        UserManagers.getVipSubscription(user_id, vipSub);
        bool isVip = (vipSub == 1);

        if (isVip) {
            // VIP не получает обычное предложение смены плана.
            // Если прошло >= 90 дней — показываем диалог «свяжитесь с тренером».
            planChangeDue = false;
            if (planResetDue) {
                vipUpdateNeeded = true;
            }
            // VIP-программу автоматически НЕ сбрасываем — её обновляет тренер.
            planResetDue = false;
        } else if (planResetDue) {
            // Lite/Plus, прошло >= 90 дней — авто-сброс плана и прогресса:
            // переиспользуем adminResetUserPlan, который чистит PlanTrainning,
            // прогресс по неделям и plan_start_date. После следующего входа
            // пользователь сам выберет новый план.
            UserManagers.adminResetUserPlan(user_id);
            qDebug() << "♻️ 90-day plan reset triggered for user" << user_id;

            // Перечитываем состояние, чтобы клиент увидел свежие нули.
            UserManagers.getWorkoutProgress(user_id, current_week, w1, w2, w3,
                                            d1, d2, d3, plan_completed);
            planChangeDue = false;
            planResetDue  = false;
        }

        // Формат: SUCCESS|week|w1|w2|w3|plan_completed|plan_change_due|vip_update_needed
        // 7-е поле — новое; старые клиенты его игнорируют.
        QString resp = QString("GET_WORKOUT_PROGRESS_SUCCESS|%1|%2|%3|%4|%5|%6|%7")
                           .arg(current_week)
                           .arg(w1 ? 1 : 0)
                           .arg(w2 ? 1 : 0)
                           .arg(w3 ? 1 : 0)
                           .arg(plan_completed ? 1 : 0)
                           .arg(planChangeDue ? 1 : 0)
                           .arg(vipUpdateNeeded ? 1 : 0);
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

    int kachballsAwarded = 0;
    if (UserManagers.markWorkoutDone(user_id, workout_num, date, kachballsAwarded)) {
        // Сохраняем снапшот статистики за текущую неделю
        UserManagers.computeAndSaveWeeklyStats(user_id);

        // Возвращаем свежее состояние прогресса
        int  current_week;
        bool w1, w2, w3, plan_completed;
        QString d1, d2, d3;
        UserManagers.getWorkoutProgress(user_id, current_week, w1, w2, w3, d1, d2, d3, plan_completed);

        bool planChangeDue = false;
        UserManagers.isPlanChangeDue(user_id, planChangeDue);

        int totalKachballs = 0;
        UserManagers.getUserKachballs(user_id, totalKachballs);

        QString resp = QString("MARK_WORKOUT_DONE_SUCCESS|%1|%2|%3|%4|%5|%6|%7|%8")
                           .arg(current_week)
                           .arg(w1 ? 1 : 0)
                           .arg(w2 ? 1 : 0)
                           .arg(w3 ? 1 : 0)
                           .arg(plan_completed ? 1 : 0)
                           .arg(planChangeDue ? 1 : 0)
                           .arg(kachballsAwarded)
                           .arg(totalKachballs);
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
