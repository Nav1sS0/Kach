#ifndef SERVER_H
#define SERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include <QList>
#include <QMap>
#include <QTimer>
#include <QDateTime>
#include "UserManager.h"

struct SocketInfo {
    QTcpSocket* socket;
    bool authenticated;
    bool adminAuthenticated = false;     // ✅ Отдельный флаг для админ-команд
    int userId = -1;
    QByteArray buffer;
    quint32 expectedSize;
    QString clientIp;
    QDateTime connectionTime;
    QDateTime lastActivityTime;
    qint64 bytesReceived = 0;
    int packetCount = 0;

    // ✅ Per-socket image upload state (исправляет race condition при параллельных загрузках)
    QByteArray imageBuffer;
    quint64 expectedImageSize = 0;
    bool isReceivingImage = false;
    int imageUserId = -1;
    int imageMeasurementId = -1;
    int imageRecipeId = -1;          // ✅ для COOKED_DISH (фото блюда)
    QString imagePhotoType;          // "front"|"side"|"back"|"dish"|"chat"
    bool imageFromAdmin = false;     // ✅ для chat-фото: кто отправитель

    SocketInfo() : socket(nullptr), authenticated(false), userId(-1), expectedSize(0), bytesReceived(0), packetCount(0) {}
};

class Server : public QTcpServer
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr);
    ~Server();

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    void slotReadyRead();
    void removeSocket();
    void checkInactiveConnections();  // ✅ Новый слот
    void checkExpiredFreezes();       // ✅ Авто-разморозка по истечении срока

private:
    // === DDoS Protection ===
    static constexpr int MAX_TOTAL_CONNECTIONS = 200;
    static constexpr int MAX_CONNECTIONS_PER_IP = 20;
    static constexpr int CONNECTION_TIMEOUT_MS = 300000;      // 5 мин на аутентификацию
    static constexpr int INACTIVITY_TIMEOUT_MS = 18000000;     // 5 часов неактивности
    static constexpr qint64 MAX_BYTES_PER_SECOND = 10485760;  // 10 MB/s
    static constexpr int MAX_PACKETS_PER_SECOND = 1000;
    // ✅ Жёсткий лимит размера пакета (защита от memory-DoS через подделку quint32 в заголовке)
    static constexpr quint32 MAX_PACKET_SIZE = 200 * 1024 * 1024;   // 200 MB
    static constexpr quint64 MAX_IMAGE_SIZE  = 100 * 1024 * 1024;   // 100 MB

    bool isRateLimited(const QString& clientIp);
    // ✅ Проверки авторизации
    bool requireAuth(QTcpSocket* sock, const SocketInfo& info, const QString& cmdName);
    bool requireOwnership(QTcpSocket* sock, const SocketInfo& info, int requestedUserId, const QString& cmdName);

    QMap<QString, int> ipConnectionCount;
    QMap<QString, QVector<qint64>> ipPacketTimestamps;
    QMap<QString, QVector<qint64>> adminLoginFailures;   // ✅ bruteforce-протекция админ-логина
    QTimer* inactivityCheckTimer;
    QTimer* freezeCheckTimer;     // ✅ Таймер авто-разморозки (раз в час)
    // === End DDoS Protection ===

    void SendToClient(QTcpSocket* clientSocket, const QString& str);
    void SendImageToClient(QTcpSocket* clientSocket, int user_id, const QByteArray& imageData);
    void SendImageToClient(QTcpSocket* clientSocket, int measurement_id, const QByteArray& imageData, const QString& photoType);
    void processCommand(QTcpSocket* currentSocket, const QString& command);
    // Завершить приём изображения (фото замера / аватар / блюдо / chat).
    // Вызывается и из slotReadyRead (когда бинарь пришёл следующими пакетами),
    // и сразу после processCommand (когда бинарь пришёл одним пакетом с заголовком).
    void finalizeIncomingImage(SocketInfo& si, QTcpSocket* sock);
    void handleGetWorkoutProgress(QTcpSocket* currentSocket, const QString& command);
    void handleMarkWorkoutDone(QTcpSocket* currentSocket, const QString& command);
    int findSocketIndex(QTcpSocket* sock);
    QTcpSocket* findSocketByUserId(int userId);
    bool checkCredentials(const QString& login, const QString& password);

    QList<SocketInfo> Sockets;
    UserManager UserManagers;

    quint32 requireSize;
    bool comlexData;
};

#endif // SERVER_H
