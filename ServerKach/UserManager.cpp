#include "UserManager.h"
#include <QDebug>
#include <QSqlQuery>
#include <QLatin1String>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSqlError>
#include <QCryptographicHash>
#include <QDateTime>

// ⚠️ ТЕСТОВЫЙ РЕЖИМ: раскомментируй строку ниже для тестирования
// Тест: неделя = 2 мин, 2 месяца плана = 16 мин
// #define TEST_WEEK_ADVANCE

namespace {

QString escapeSqlLiteral(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\''), QLatin1String("''"));
    return out;
}

} // namespace

UserManager::UserManager() {

    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName("./../../../kach_users.db");

    if(db.open())
    {
        qDebug() << "Data Base open";

        // WAL режим - предотвращает блокировку при одновременном доступе
        QSqlQuery walQuery(db);
        walQuery.exec("PRAGMA journal_mode = WAL");

        // Включаем поддержку внешних ключей
        QSqlQuery pragmaQuery(db);
        pragmaQuery.exec("PRAGMA foreign_keys = ON");

        // Создаём все таблицы тренировок если их нет
        initializeTables();
    }
    else
    {
        qDebug() << "Error: " << db.lastError().databaseText();
    }
}

void UserManager::initializeTables()
{
    QStringList tables = {
        "Save_Training_Buttock_Bridge_P1",
        "Save_Training_Chest_Press_P1",
        "Save_Training_Horizontal_Thrust_P1",
        "Save_Training_One_Leg_Bench_Press_P1",
        "Save_Training_Press_On_Mat_P1",
        "Save_Training_Vertical_Thrust_P1",
        "Save_Training_Press_On_Ball_P1",
        "Save_Training_Kettlebell_Squat_P1",
        "Save_Training_Romanian_Deadlift_P1",
        "Save_Training_Knee_Pushup_P1",
        "Save_Training_Hip_Abduction_P1",
        "Save_Training_Tricep_Extension_P1",
        "Save_Training_Assisted_Pullup_P1",
        "Save_Training_Bulgarian_Split_Squat_P1",
        "Save_Training_Shoulder_Press_P1",
        "Save_Training_Cable_Fly_P1",
        "Save_Training_Leg_Extension_P1",
        "Save_Training_Plank_P1",
        "Save_Training_Chest_Fly_P1",
        // Plan 2 - новые упражнения
        "Save_Training_Hip_Abduction_Lean_P2",
        "Save_Training_Pushup_P2",
        "Save_Training_Hip_Abduction_90_P2",
        "Save_Training_Pullup_P2",
        "Save_Training_Leg_Extension_Frog_P2",
        // Plan 3 — 9 новых упражнений
        "Save_Training_Crane_P3",
        "Save_Training_Shoulder_Side_Raise_Sit_P3",
        "Save_Training_Goodmorning_Hack_P3",
        "Save_Training_Shoulder_Raise_One_Hand_Bench_P3",
        "Save_Training_Froggy_Stand_P3",
        "Save_Training_Hack_Squat_P3",
        "Save_Training_Pullover_P3",
        "Save_Training_Crossover_Diagonal_Kick_P3",
        "Save_Training_Wall_Abduction_One_P3",
        // Plan 4 — 6 новых упражнений
        "Save_Training_Cable_Kickback_P4",
        "Save_Training_Crossover_Step_Up_P4",
        "Save_Training_Dips_P4",
        "Save_Training_Cable_Stork_P4",
        "Save_Training_Glute_Bridge_Single_Leg_P4",
        "Save_Training_Cable_Rotation_P4",
        // Plan 7 — 4 новых упражнения
        "Save_Training_Negative_Pullup_P7",
        "Save_Training_Barbell_Bicep_Curl_P7",
        "Save_Training_Leg_Raise_Elbow_P7",
        "Save_Training_Brachialis_Curl_P7"
    };

    for (const QString &table : tables) {
        QSqlQuery q(db);
        QString sql = QString(
                          "CREATE TABLE IF NOT EXISTS \"%1\" ("
                          "\"id\" INTEGER, "
                          "\"user_id\" INTEGER, "
                          "\"Approach1\" REAL DEFAULT 0, "
                          "\"Approach2\" REAL DEFAULT 0, "
                          "\"Approach3\" REAL DEFAULT 0, "
                          "PRIMARY KEY(\"id\" AUTOINCREMENT), "
                          "FOREIGN KEY(\"user_id\") REFERENCES \"USERS\")"
                          ).arg(table);

        if (!q.exec(sql)) {
            qWarning() << "Failed to create table" << table << ":" << q.lastError().text();
        } else {
            qDebug() << "Table ready:" << table;
        }
    }

    // ✅ Таблица прогресса по неделям
    QSqlQuery wq(db);
    wq.exec(
        "CREATE TABLE IF NOT EXISTS Training_Week_Progress ("
        "  user_id         INTEGER PRIMARY KEY, "
        "  current_week    INTEGER DEFAULT 1, "
        "  workout1_done   INTEGER DEFAULT 0, "
        "  workout2_done   INTEGER DEFAULT 0, "
        "  workout3_done   INTEGER DEFAULT 0, "
        "  w1_date         TEXT DEFAULT '', "
        "  w2_date         TEXT DEFAULT '', "
        "  w3_date         TEXT DEFAULT '', "
        "  week_start_date TEXT DEFAULT '', "
        "  week_completed  INTEGER DEFAULT 0, "
        "  FOREIGN KEY(user_id) REFERENCES USERS)"
        );
    if (wq.lastError().isValid())
        qWarning() << "Failed to create Training_Week_Progress:" << wq.lastError().text();
    else
        qDebug() << "Table ready: Training_Week_Progress";

    // Миграции (игнорируем ошибку — если колонка уже есть, SQLite вернёт ошибку, это нормально)
    QSqlQuery mig(db);
    mig.exec("ALTER TABLE Training_Week_Progress ADD COLUMN week_completed INTEGER DEFAULT 0");
    mig.exec("ALTER TABLE Training_Week_Progress ADD COLUMN plan_start_date TEXT DEFAULT ''");

    // ── Миграция: переименование Neck_Circumference → Chest_Circumference ──
    // По ТЗ замер называется «обхват груди». Старая колонка Neck_Circumference
    // фактически использовалась для груди (label в UI был "Обхват груди"), но
    // имя поля было ошибочным. Добавляем новую колонку и копируем туда данные;
    // старая колонка остаётся в БД для безопасности (просто не используется).
    QSqlQuery measMig(db);
    measMig.exec("ALTER TABLE UserMeasurements ADD COLUMN Chest_Circumference REAL");
    measMig.exec(
        "UPDATE UserMeasurements "
        "SET Chest_Circumference = Neck_Circumference "
        "WHERE Chest_Circumference IS NULL AND Neck_Circumference IS NOT NULL");

    // ✅ Таблица рецептов (img — BLOB для хранения изображений)
    QSqlQuery rq(db);
    if (!rq.exec(
            "CREATE TABLE IF NOT EXISTS Recipes ("
            "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name       TEXT    NOT NULL, "
            "meal_type  TEXT    NOT NULL, "
            "kcal       INTEGER DEFAULT 0, "
            "protein    INTEGER DEFAULT 0, "
            "fat        INTEGER DEFAULT 0, "
            "carbs      INTEGER DEFAULT 0, "
            "rating     REAL    DEFAULT 0.0, "
            "ingredient TEXT    DEFAULT '', "
            "img        BLOB    DEFAULT NULL)")) {
        qWarning() << "Failed to create Recipes:" << rq.lastError().text();
    } else {
        qDebug() << "Table ready: Recipes";
    }

    // ✅ Таблица ингредиентов рецептов
    QSqlQuery iq(db);
    if (!iq.exec(
            "CREATE TABLE IF NOT EXISTS Recipe_Ingredients ("
            "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
            "recipe_id       INTEGER NOT NULL, "
            "ingredient_name TEXT    NOT NULL, "
            "amount          TEXT    NOT NULL, "
            "FOREIGN KEY(recipe_id) REFERENCES Recipes(id) ON DELETE CASCADE)")) {
        qWarning() << "Failed to create Recipe_Ingredients:" << iq.lastError().text();
    } else {
        qDebug() << "Table ready: Recipe_Ingredients";
    }

    // ✅ Таблица шагов приготовления
    QSqlQuery sq(db);
    if (!sq.exec(
            "CREATE TABLE IF NOT EXISTS Recipe_Steps ("
            "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
            "recipe_id  INTEGER NOT NULL, "
            "step_order INTEGER NOT NULL, "
            "step_text  TEXT    NOT NULL, "
            "FOREIGN KEY(recipe_id) REFERENCES Recipes(id) ON DELETE CASCADE)")) {
        qWarning() << "Failed to create Recipe_Steps:" << sq.lastError().text();
    } else {
        qDebug() << "Table ready: Recipe_Steps";
    }

    // ✅ Таблица геймификации
    QSqlQuery gamifQ(db);
    if (!gamifQ.exec(
            "CREATE TABLE IF NOT EXISTS Gamification ("
            "id         INTEGER PRIMARY KEY CHECK(id=1), "
            "start_date TEXT    NOT NULL DEFAULT (date('now')), "
            "total_days INTEGER NOT NULL DEFAULT 90)")) {
        qWarning() << "Failed to create Gamification:" << gamifQ.lastError().text();
    } else {
        qDebug() << "Table ready: Gamification";
        // Вставить запись-одиночку если её нет
        QSqlQuery initG(db);
        initG.exec("INSERT OR IGNORE INTO Gamification (id, start_date, total_days) VALUES (1, date('now'), 90)");
    }

    // ✅ Заполнить начальные рецепты (только если таблица пустая)
    seedInitialRecipes();

    // ✅ Таблица отзывов к рецептам
    QSqlQuery revq(db);
    if (!revq.exec(
            "CREATE TABLE IF NOT EXISTS Recipe_Reviews ("
            "id          INTEGER PRIMARY KEY AUTOINCREMENT, "
            "recipe_id   INTEGER NOT NULL, "
            "user_id     INTEGER NOT NULL, "
            "user_name   TEXT    NOT NULL DEFAULT 'Пользователь', "
            "rating      INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 5), "
            "comment     TEXT    DEFAULT '', "
            "created_at  TEXT    NOT NULL DEFAULT (datetime('now')), "
            "FOREIGN KEY(recipe_id) REFERENCES Recipes(id) ON DELETE CASCADE, "
            "UNIQUE(recipe_id, user_id))")) {
        qWarning() << "Failed to create Recipe_Reviews:" << revq.lastError().text();
    } else {
        qDebug() << "Table ready: Recipe_Reviews";
    }

    // ✅ Таблица дневного рациона (КБЖУ по дням)
    QSqlQuery dnq(db);
    if (!dnq.exec(
            "CREATE TABLE IF NOT EXISTS Daily_Nutrition_Log ("
            "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
            "user_id    INTEGER NOT NULL, "
            "log_date   TEXT    NOT NULL, "
            "meals_json TEXT    DEFAULT '[]', "
            "total_kcal    INTEGER DEFAULT 0, "
            "total_protein INTEGER DEFAULT 0, "
            "total_fat     INTEGER DEFAULT 0, "
            "total_carbs   INTEGER DEFAULT 0, "
            // В БД таблица users использует primary key = user_id,
            // поэтому внешний ключ должен ссылаться на USERS(user_id),
            // иначе SQLite отдаёт "foreign key mismatch" и INSERT/DELETE не работают.
            "FOREIGN KEY(user_id) REFERENCES USERS(user_id), "
            "UNIQUE(user_id, log_date))")) {
        qWarning() << "Failed to create Daily_Nutrition_Log:" << dnq.lastError().text();
    } else {
        qDebug() << "Table ready: Daily_Nutrition_Log";
    }

    // Миграция для уже существующей БД:
    // ранее FK был сделан как REFERENCES USERS(id), что не соответствует текущей схеме USERS(user_id).
    // В этом случае SAVE_DAILY_NUTRITION будет падать с foreign key mismatch.
    {
        QSqlQuery chk(db);
        if (chk.exec("SELECT sql FROM sqlite_master WHERE type='table' AND name='Daily_Nutrition_Log'")) {
            if (chk.next()) {
                const QString createSql = chk.value(0).toString();
                if (createSql.contains("REFERENCES USERS(id)") ||
                    createSql.contains("REFERENCES \"USERS\"(id)") ||
                    createSql.contains("\"USERS\"(id)") ||
                    createSql.contains("USERS(id)")) {
                    qDebug() << "Daily_Nutrition_Log FK mismatch detected, migrating...";

                    if (db.transaction()) {
                        QSqlQuery rn(db);
                        if (rn.exec("ALTER TABLE Daily_Nutrition_Log RENAME TO Daily_Nutrition_Log_old")) {
                            QSqlQuery createNew(db);
                            const QString createNewSql =
                                "CREATE TABLE IF NOT EXISTS Daily_Nutrition_Log ("
                                "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
                                "user_id    INTEGER NOT NULL, "
                                "log_date   TEXT    NOT NULL, "
                                "meals_json TEXT    DEFAULT '[]', "
                                "total_kcal    INTEGER DEFAULT 0, "
                                "total_protein INTEGER DEFAULT 0, "
                                "total_fat     INTEGER DEFAULT 0, "
                                "total_carbs   INTEGER DEFAULT 0, "
                                "FOREIGN KEY(user_id) REFERENCES USERS(user_id), "
                                "UNIQUE(user_id, log_date))";

                            if (createNew.exec(createNewSql)) {
                                QSqlQuery copy(db);
                                const QString copySql =
                                    "INSERT INTO Daily_Nutrition_Log (id, user_id, log_date, meals_json, "
                                    "total_kcal, total_protein, total_fat, total_carbs) "
                                    "SELECT id, user_id, log_date, meals_json, "
                                    "total_kcal, total_protein, total_fat, total_carbs "
                                    "FROM Daily_Nutrition_Log_old";

                                if (!copy.exec(copySql)) {
                                    qWarning() << "Daily_Nutrition_Log migration copy failed:" << copy.lastError().text();
                                    db.rollback();
                                } else {
                                    QSqlQuery drop(db);
                                    if (!drop.exec("DROP TABLE Daily_Nutrition_Log_old")) {
                                        qWarning() << "Daily_Nutrition_Log migration drop failed:" << drop.lastError().text();
                                        db.rollback();
                                    } else {
                                        db.commit();
                                        qDebug() << "Daily_Nutrition_Log migration done.";
                                    }
                                }
                            } else {
                                qWarning() << "Daily_Nutrition_Log migration create failed:" << createNew.lastError().text();
                                db.rollback();
                            }
                        } else {
                            qWarning() << "Daily_Nutrition_Log migration rename failed:" << rn.lastError().text();
                            db.rollback();
                        }
                    } else {
                        qWarning() << "Daily_Nutrition_Log migration: transaction begin failed";
                    }
                }
            }
        }
    }

    // ── Статистика тренировок по неделям ────────────────────────────────────
    QSqlQuery statsQ(db);
    if (!statsQ.exec(
            "CREATE TABLE IF NOT EXISTS Workout_Weekly_Stats ("
            "  user_id       INTEGER NOT NULL, "
            "  week_num      INTEGER NOT NULL, "
            "  exercise_name TEXT    NOT NULL, "
            "  avg_weight    REAL    DEFAULT 0, "
            "  PRIMARY KEY(user_id, week_num, exercise_name), "
            "  FOREIGN KEY(user_id) REFERENCES USERS(user_id))")) {
        qWarning() << "Failed to create Workout_Weekly_Stats:" << statsQ.lastError().text();
    } else {
        qDebug() << "Table ready: Workout_Weekly_Stats";
    }

    // ── Замены упражнений ───────────────────────────────────────────────────
    {
        QSqlQuery esq(db);
        if (!esq.exec(
                "CREATE TABLE IF NOT EXISTS exercise_substitutions ("
                "id              INTEGER PRIMARY KEY AUTOINCREMENT, "
                "user_id         INTEGER NOT NULL, "
                "original_name   TEXT    NOT NULL, "
                "substitute_name TEXT    NOT NULL, "
                "FOREIGN KEY(user_id) REFERENCES USERS(user_id), "
                "UNIQUE(user_id, original_name))")) {
            qWarning() << "Failed to create exercise_substitutions:" << esq.lastError().text();
        } else {
            qDebug() << "Table ready: exercise_substitutions";
        }
    }

    // ── Друзья ──────────────────────────────────────────────────────────────
    // from_user_id → to_user_id, status: 'pending' / 'accepted'
    // При принятии: обновляем pending-строку в accepted И добавляем обратную строку accepted.
    // Список друзей: SELECT to_user_id FROM Friends WHERE from_user_id=me AND status='accepted'
    {
        QSqlQuery fq(db);
        if (!fq.exec(
                "CREATE TABLE IF NOT EXISTS Friends ("
                "  id           INTEGER PRIMARY KEY AUTOINCREMENT, "
                "  from_user_id INTEGER NOT NULL, "
                "  to_user_id   INTEGER NOT NULL, "
                "  status       TEXT    NOT NULL DEFAULT 'pending', "
                "  created_at   TEXT    NOT NULL DEFAULT (datetime('now')), "
                "  FOREIGN KEY(from_user_id) REFERENCES USERS(user_id), "
                "  FOREIGN KEY(to_user_id)   REFERENCES USERS(user_id), "
                "  UNIQUE(from_user_id, to_user_id))")) {
            qWarning() << "Failed to create Friends table:" << fq.lastError().text();
        } else {
            qDebug() << "Table ready: Friends";
        }
    }

    // ── Миграция: колонка kachballs в USERS ────────────────────────────────
    {
        QSqlQuery mig(db);
        mig.exec("ALTER TABLE USERS ADD COLUMN kachballs INTEGER DEFAULT 0");
        // Ошибка = колонка уже есть, это нормально
    }

    // ── Миграция: персональная соль для пароля ────────────────────────────
    {
        QSqlQuery mig(db);
        mig.exec("ALTER TABLE USERS ADD COLUMN password_salt TEXT DEFAULT ''");
    }

    // ── Миграция: заморозка подписки (1 = заморожена, 0 = активна) ────────
    {
        QSqlQuery mig(db);
        mig.exec("ALTER TABLE USERS ADD COLUMN subscription_frozen INTEGER DEFAULT 0");
        // Авто-разморозка: дата окончания заморозки (UTC ISO)
        mig.exec("ALTER TABLE USERS ADD COLUMN freeze_ends_at TEXT DEFAULT ''");
        // Счётчик заморозок в текущем году
        mig.exec("ALTER TABLE USERS ADD COLUMN freeze_count_year INTEGER DEFAULT 0");
        mig.exec("ALTER TABLE USERS ADD COLUMN freeze_year INTEGER DEFAULT 0");
    }

    // ── Миграция: 3-й уровень подписки — "с обратной связью" ──────────────
    {
        QSqlQuery mig(db);
        mig.exec("ALTER TABLE USERS ADD COLUMN FeedbackSubscription INTEGER DEFAULT 0");
    }

    // ── Миграция: авто-биллинг (рекуррентные платежи YooKassa) ────────────
    // Сценарий: пользователь платит на сайте → YooKassa возвращает payment_method_id;
    // сайт-cron раз в день вызывает ADMIN_BILLING_DUE → списывает с сохранённой карты.
    // 3 неуспешных попытки подряд → биллинг-заморозка (без квоты 2/год).
    {
        QSqlQuery mig(db);
        // Идентификатор сохранённой карты YooKassa (для рекуррентных списаний)
        mig.exec("ALTER TABLE USERS ADD COLUMN payment_method_id TEXT DEFAULT ''");
        // Тариф для авто-биллинга: 'lite' | 'feedback' | '' (VIP не авто-продлевается)
        mig.exec("ALTER TABLE USERS ADD COLUMN subscription_tier TEXT DEFAULT ''");
        // Дата следующего автоматического списания (ISO UTC)
        mig.exec("ALTER TABLE USERS ADD COLUMN next_billing_date TEXT DEFAULT ''");
        // Счётчик подряд неудачных попыток списания (0..3)
        mig.exec("ALTER TABLE USERS ADD COLUMN billing_retry_count INTEGER DEFAULT 0");
        // Флаг "пользователь согласился на авто-продление" (0/1)
        mig.exec("ALTER TABLE USERS ADD COLUMN subscription_auto_renew INTEGER DEFAULT 0");
        // Маскированное представление карты ("**** 4242") — показываем в личном кабинете
        mig.exec("ALTER TABLE USERS ADD COLUMN payment_method_title TEXT DEFAULT ''");
        // Время последней попытки списания и её исход (для диагностики/UI)
        mig.exec("ALTER TABLE USERS ADD COLUMN last_billing_attempt_at TEXT DEFAULT ''");
        mig.exec("ALTER TABLE USERS ADD COLUMN last_billing_status TEXT DEFAULT ''");
        // Причина текущей заморозки: '' | 'manual' (admin) | 'billing' (auto-failed)
        mig.exec("ALTER TABLE USERS ADD COLUMN freeze_reason TEXT DEFAULT ''");
    }

    // ── Таблица сообщений чата (юзер ↔ админ) ─────────────────────────────
    {
        QSqlQuery q(db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS Chat_Messages ("
            "  id          INTEGER PRIMARY KEY AUTOINCREMENT, "
            "  user_id     INTEGER NOT NULL, "       // абонент (не админ)
            "  from_admin  INTEGER NOT NULL DEFAULT 0, "  // 0 = от пользователя, 1 = от админа
            "  text        TEXT    NOT NULL, "
            "  created_at  TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  read_by_user INTEGER NOT NULL DEFAULT 0, "
            "  read_by_admin INTEGER NOT NULL DEFAULT 0, "
            "  FOREIGN KEY(user_id) REFERENCES USERS(user_id))");
        q.exec("CREATE INDEX IF NOT EXISTS idx_chat_user ON Chat_Messages(user_id, created_at)");

        // Миграция: фото-сообщения в чате (BLOB в той же строке).
        // Грузится отдельной командой GET_CHAT_PHOTO|message_id, чтобы
        // история чата (loadChatHistory) не таскала бинарь каждый раз.
        q.exec("ALTER TABLE Chat_Messages ADD COLUMN photo BLOB DEFAULT NULL");
    }

    // ── Чат техподдержки (доступен ВСЕМ пользователям, не только Plus/VIP) ──
    {
        QSqlQuery q(db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS Support_Messages ("
            "  id            INTEGER PRIMARY KEY AUTOINCREMENT, "
            "  user_id       INTEGER NOT NULL, "
            "  from_admin    INTEGER NOT NULL DEFAULT 0, "
            "  text          TEXT    NOT NULL, "
            "  photo         BLOB    DEFAULT NULL, "
            "  created_at    TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  read_by_user  INTEGER NOT NULL DEFAULT 0, "
            "  read_by_admin INTEGER NOT NULL DEFAULT 0, "
            "  FOREIGN KEY(user_id) REFERENCES USERS(user_id))");
        q.exec("CREATE INDEX IF NOT EXISTS idx_support_user ON Support_Messages(user_id, created_at)");
    }

    // ── Приготовленные блюда (фото блюда → баллы) ─────────────────────────
    {
        QSqlQuery q(db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS Cooked_Dishes ("
            "  id          INTEGER PRIMARY KEY AUTOINCREMENT, "
            "  user_id     INTEGER NOT NULL, "
            "  recipe_id   INTEGER, "
            "  photo       BLOB, "
            "  created_at  TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  approved    INTEGER NOT NULL DEFAULT 1, "
            "  FOREIGN KEY(user_id) REFERENCES USERS(user_id), "
            "  FOREIGN KEY(recipe_id) REFERENCES Recipes(id))");
        q.exec("CREATE INDEX IF NOT EXISTS idx_cooked_user ON Cooked_Dishes(user_id, created_at)");
    }

    // ── Просмотренные уроки (для начисления баллов) ───────────────────────
    {
        QSqlQuery q(db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS User_Lessons_Viewed ("
            "  id          INTEGER PRIMARY KEY AUTOINCREMENT, "
            "  user_id     INTEGER NOT NULL, "
            "  lesson_key  TEXT    NOT NULL, "
            "  viewed_at   TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  UNIQUE(user_id, lesson_key))");
    }

    // ── Избранные рецепты пользователя ────────────────────────────────────
    {
        QSqlQuery q(db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS User_Favorite_Recipes ("
            "  user_id    INTEGER NOT NULL, "
            "  recipe_id  INTEGER NOT NULL, "
            "  added_at   TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  PRIMARY KEY(user_id, recipe_id))");
    }

    // ── Устройства пользователей для push-уведомлений (FCM/APNS) ──────────
    {
        QSqlQuery q(db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS User_Devices ("
            "  user_id      INTEGER NOT NULL, "
            "  device_token TEXT    NOT NULL, "
            "  platform     TEXT    NOT NULL, "    // "ios" | "android"
            "  last_seen    TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  PRIMARY KEY(user_id, device_token))");
    }

    // ── Каталог достижений ─────────────────────────────────────────────────
    {
        QSqlQuery aq(db);
        if (!aq.exec(
                "CREATE TABLE IF NOT EXISTS Achievements ("
                "  id          INTEGER PRIMARY KEY AUTOINCREMENT, "
                "  key         TEXT    NOT NULL UNIQUE, "
                "  name        TEXT    NOT NULL, "
                "  description TEXT    NOT NULL DEFAULT '', "
                "  category    TEXT    NOT NULL DEFAULT 'training', "
                "  reward      INTEGER NOT NULL DEFAULT 10, "
                "  threshold   INTEGER NOT NULL DEFAULT 1)")) {
            qWarning() << "Failed to create Achievements:" << aq.lastError().text();
        } else {
            qDebug() << "Table ready: Achievements";
        }
    }

    // ── Полученные достижения пользователей ────────────────────────────────
    {
        QSqlQuery uaq(db);
        if (!uaq.exec(
                "CREATE TABLE IF NOT EXISTS User_Achievements ("
                "  id             INTEGER PRIMARY KEY AUTOINCREMENT, "
                "  user_id        INTEGER NOT NULL, "
                "  achievement_id INTEGER NOT NULL, "
                "  earned_at      TEXT    NOT NULL DEFAULT (datetime('now')), "
                "  FOREIGN KEY(user_id)        REFERENCES USERS(user_id), "
                "  FOREIGN KEY(achievement_id) REFERENCES Achievements(id), "
                "  UNIQUE(user_id, achievement_id))")) {
            qWarning() << "Failed to create User_Achievements:" << uaq.lastError().text();
        } else {
            qDebug() << "Table ready: User_Achievements";
        }
    }

    // ── VIP Персональные программы ──────────────────────────────────────────
    {
        QSqlQuery vp(db);
        vp.exec(
            "CREATE TABLE IF NOT EXISTS Vip_Programs ("
            "  id            INTEGER PRIMARY KEY AUTOINCREMENT, "
            "  user_id       INTEGER UNIQUE NOT NULL, "
            "  program_name  TEXT NOT NULL, "
            "  workout1_json TEXT NOT NULL DEFAULT '[]', "
            "  workout2_json TEXT NOT NULL DEFAULT '[]', "
            "  workout3_json TEXT NOT NULL DEFAULT '[]', "
            "  created_at    TEXT NOT NULL, "
            "  FOREIGN KEY(user_id) REFERENCES USERS(user_id)"
            ")");
        if (vp.lastError().isValid())
            qWarning() << "Failed to create Vip_Programs:" << vp.lastError().text();
        else
            qDebug() << "Table ready: Vip_Programs";

        QSqlQuery vw(db);
        vw.exec(
            "CREATE TABLE IF NOT EXISTS Vip_Exercise_Weights ("
            "  id            INTEGER PRIMARY KEY AUTOINCREMENT, "
            "  user_id       INTEGER NOT NULL, "
            "  exercise_name TEXT NOT NULL, "
            "  approach1     REAL DEFAULT 0, "
            "  approach2     REAL DEFAULT 0, "
            "  approach3     REAL DEFAULT 0, "
            "  UNIQUE(user_id, exercise_name)"
            ")");
        if (vw.lastError().isValid())
            qWarning() << "Failed to create Vip_Exercise_Weights:" << vw.lastError().text();
        else
            qDebug() << "Table ready: Vip_Exercise_Weights";

        // Add VipSubscription column to USERS if not exists
        QSqlQuery alter(db);
        alter.exec("ALTER TABLE USERS ADD COLUMN VipSubscription INTEGER DEFAULT 0");
        // Ignore error if column already exists
    }

    // ── Заполнить каталог достижений (один раз) ────────────────────────────
    seedAchievements();

    // ── Настройки призовых баллов (редактируются админом) ───────────────────
    {
        QSqlQuery bs(db);
        if (!bs.exec(
                "CREATE TABLE IF NOT EXISTS Bonus_Settings ("
                "  key         TEXT PRIMARY KEY, "
                "  amount      INTEGER NOT NULL DEFAULT 0, "
                "  description TEXT NOT NULL DEFAULT '')")) {
            qWarning() << "Failed to create Bonus_Settings:" << bs.lastError().text();
        } else {
            qDebug() << "Table ready: Bonus_Settings";
        }

        struct BonusDef { QString key; int amount; QString desc; };
        const QVector<BonusDef> defs = {
            { "workout_done",  15, "За завершённую тренировку" },
            { "dish_added",     5, "За добавление блюда в рацион" },
            { "cooked_dish",    5, "За загруженное фото блюда" },
            { "lesson_viewed",  3, "За просмотренный урок" },
        };
        for (const auto &d : defs) {
            QSqlQuery ins(db);
            ins.prepare("INSERT OR IGNORE INTO Bonus_Settings (key, amount, description) "
                        "VALUES (:k, :a, :d)");
            ins.bindValue(":k", d.key);
            ins.bindValue(":a", d.amount);
            ins.bindValue(":d", d.desc);
            ins.exec();
        }
    }

    // ── Дедуп начислений за добавленные блюда (один день — один рецепт) ─────
    {
        QSqlQuery dd(db);
        dd.exec(
            "CREATE TABLE IF NOT EXISTS Diet_Reward_Log ("
            "  user_id     INTEGER NOT NULL, "
            "  date        TEXT    NOT NULL, "
            "  recipe_name TEXT    NOT NULL, "
            "  rewarded_at TEXT    NOT NULL DEFAULT (datetime('now')), "
            "  PRIMARY KEY(user_id, date, recipe_name))");
    }

    // ── Еженедельные миссии ─────────────────────────────────────────────────
    {
        QSqlQuery wm(db);
        wm.exec(
            "CREATE TABLE IF NOT EXISTS Weekly_Missions ("
            "  user_id            INTEGER NOT NULL, "
            "  year               INTEGER NOT NULL, "
            "  week_num           INTEGER NOT NULL, "
            "  steps_done         INTEGER NOT NULL DEFAULT 0, "
            "  workouts_done      INTEGER NOT NULL DEFAULT 0, "
            "  nutrition_done     INTEGER NOT NULL DEFAULT 0, "
            "  steps_rewarded     INTEGER NOT NULL DEFAULT 0, "
            "  workouts_rewarded  INTEGER NOT NULL DEFAULT 0, "
            "  nutrition_rewarded INTEGER NOT NULL DEFAULT 0, "
            "  full_bonus_rewarded INTEGER NOT NULL DEFAULT 0, "
            "  PRIMARY KEY (user_id, year, week_num)"
            ")");
        if (wm.lastError().isValid())
            qWarning() << "Failed to create Weekly_Missions:" << wm.lastError().text();
        else
            qDebug() << "Table ready: Weekly_Missions";
    }
}

// ---------------------------------------------------------------------------
// Заполнение каталога достижений
// ---------------------------------------------------------------------------
void UserManager::seedAchievements()
{
    QSqlQuery countQ(db);
    if (!countQ.exec("SELECT COUNT(*) FROM Achievements")) return;
    if (countQ.next() && countQ.value(0).toInt() > 0) {
        qDebug() << "Achievements already seeded, skipping.";
        return;
    }

    // { key, name, description, category, reward, threshold }
    struct AchDef { QString key; QString name; QString desc; QString cat; int reward; int threshold; };
    QVector<AchDef> defs = {
        // ── Тренировочные ──
        {"first_workout",      "Первый шаг",            "Завершите первую тренировку",                  "training", 10,  1},
        {"workouts_10",        "Новичок",               "Завершите 10 тренировок",                      "training", 25,  10},
        {"workouts_50",        "Опытный атлет",         "Завершите 50 тренировок",                      "training", 50,  50},
        {"workouts_100",       "Железная воля",         "Завершите 100 тренировок",                     "training", 100, 100},
        {"week_complete",      "Неделя выполнена",      "Выполните все 3 тренировки за неделю",          "training", 20,  1},
        {"plan_complete",      "План завершён",         "Полностью завершите тренировочный план",        "training", 75,  1},
        {"weeks_4",            "Месяц дисциплины",      "Завершите 4 недели тренировок",                 "training", 40,  4},

        // ── Социальные ──
        {"first_friend",       "Первый друг",           "Добавьте первого друга",                       "social",   10,  1},
        {"friends_5",          "Компания",              "Добавьте 5 друзей",                            "social",   25,  5},
        {"friends_10",         "Популярный",            "Добавьте 10 друзей",                           "social",   50,  10},
        {"first_review",       "Критик",                "Оставьте первый отзыв на рецепт",             "social",   10,  1},
        {"reviews_5",          "Гурман-критик",         "Оставьте 5 отзывов на рецепты",               "social",   25,  5},

        // ── Замеры и питание ──
        {"first_measurement",  "Первый замер",          "Сделайте первый замер тела",                   "body",     10,  1},
        {"measurements_10",    "Аналитик тела",         "Сделайте 10 замеров",                          "body",     30,  10},
        {"nutrition_7_days",   "Неделя питания",        "Заполняйте рацион 7 дней подряд",              "nutrition",30,  7},
        {"nutrition_30_days",  "Месяц питания",         "Заполняйте рацион 30 дней подряд",             "nutrition",75,  30},
        {"first_nutrition",    "Первый рацион",         "Заполните рацион впервые",                     "nutrition",10,  1},
    };

    for (const auto& a : defs) {
        QSqlQuery ins(db);
        ins.prepare("INSERT OR IGNORE INTO Achievements (key, name, description, category, reward, threshold) "
                    "VALUES (:key, :name, :desc, :cat, :reward, :thr)");
        ins.bindValue(":key",    a.key);
        ins.bindValue(":name",   a.name);
        ins.bindValue(":desc",   a.desc);
        ins.bindValue(":cat",    a.cat);
        ins.bindValue(":reward", a.reward);
        ins.bindValue(":thr",    a.threshold);
        if (!ins.exec())
            qWarning() << "Failed to seed achievement" << a.key << ":" << ins.lastError().text();
    }
    qDebug() << "Achievements seeded:" << defs.size() << "entries";
}

// ---------------------------------------------------------------------------
// Заполнение начальных рецептов (вызывается один раз при пустой таблице)
// ---------------------------------------------------------------------------
void UserManager::seedInitialRecipes()
{
    QSqlQuery countQ(db);
    if (!countQ.exec("SELECT COUNT(*) FROM Recipes")) {
        qWarning() << "seedInitialRecipes: cannot query Recipes:" << countQ.lastError().text();
        return;
    }
    if (countQ.next() && countQ.value(0).toInt() > 0) {
        qDebug() << "Recipes already seeded, skipping.";
        return;
    }

    // Структура: { name, mealType, kcal, protein, fat, carbs, rating, ingredient, img,
    //              ingredients [[name,amount],...], steps [...] }
    struct RecipeSeed {
        QString name, mealType, ingredient, img;
        int kcal, protein, fat, carbs;
        double rating;
        QVector<QPair<QString,QString>> ingredients;
        QStringList steps;
    };

    QVector<RecipeSeed> seeds = {
        { "Банановые вафли", "Завтрак", "банан", " ", 221, 10, 6, 35, 5.0,
         {{"Творог 5%","70 гр"},{"Банан","60 гр"},{"Мука рисовая","35 гр"},{"Желток","1 шт"},{"Разрыхлитель","1 чл"},{"Подсластитель","по вкусу"}},
         {"Размять банан вилкой до однородной массы.","Смешать творог, желток и подсластитель с бананом.","Добавить рисовую муку и разрыхлитель, перемешать.","Разогреть вафельницу и смазать маслом.","Выпекать вафли 4–5 минут до золотистого цвета.","Подавать тёплыми с ягодами или мёдом."} },

        { "Овсяная каша", "Завтрак", "овёс", " ", 180, 8, 4, 30, 4.8,
         {{"Овсяные хлопья","60 гр"},{"Молоко 1.5%","200 мл"},{"Мёд","1 чл"},{"Ягоды","30 гр"},{"Соль","щепотка"}},
         {"Залить хлопья молоком в кастрюле.","Варить на среднем огне 5 минут, помешивая.","Добавить соль и мёд по вкусу.","Разложить по тарелкам и украсить ягодами."} },

        { "Омлет с овощами", "Завтрак", "яйца", " ", 260, 18, 14, 8, 4.7,
         {{"Яйца","3 шт"},{"Помидор","1 шт"},{"Болгарский перец","½ шт"},{"Шпинат","20 гр"},{"Масло оливковое","1 чл"},{"Соль, перец","по вкусу"}},
         {"Взбить яйца с солью и перцем.","Нарезать овощи небольшими кубиками.","Обжарить овощи на оливковом масле 2 мин.","Влить яйца, накрыть крышкой и готовить 3 мин.","Сложить омлет пополам и подавать."} },

        { "Тост с авокадо", "Завтрак", "авокадо", " ", 340, 12, 20, 28, 4.9,
         {{"Хлеб цельнозерновой","2 ломтика"},{"Авокадо","½ шт"},{"Лимонный сок","1 чл"},{"Яйцо пашот","1 шт"},{"Соль, перец","по вкусу"}},
         {"Поджарить хлеб в тостере.","Размять авокадо с лимонным соком, солью и перцем.","Приготовить яйцо пашот в кипящей воде 3 мин.","Намазать авокадо на тост, сверху положить яйцо.","Посыпать перцем и подавать сразу."} },

        { "Гречка с курицей", "Обед", "гречка", " ", 410, 35, 8, 48, 4.9,
         {{"Гречка","100 гр"},{"Куриное филе","150 гр"},{"Лук репчатый","1 шт"},{"Масло раст.","1 чл"},{"Соль, специи","по вкусу"}},
         {"Отварить гречку в подсоленной воде 20 мин.","Нарезать куриное филе кубиками.","Обжарить лук до золотистого цвета.","Добавить курицу, обжарить 7–8 мин.","Смешать гречку с курицей и луком.","Приправить специями и подавать."} },

        { "Борщ", "Обед", "свёкла", " ", 320, 15, 10, 38, 4.6,
         {{"Свёкла","2 шт"},{"Капуста","200 гр"},{"Морковь","1 шт"},{"Картофель","2 шт"},{"Говядина","150 гр"},{"Томатная паста","1 ст.л"},{"Соль, лавровый лист","по вкусу"}},
         {"Сварить бульон из говядины 1 час.","Добавить картофель, варить 10 мин.","Нашинковать капусту, натереть морковь и свёклу.","Добавить овощи в бульон, тушить 15 мин.","Добавить томатную пасту, соль и лавровый лист.","Настоять 10 мин перед подачей."} },

        { "Куриный суп", "Обед", "курица", " ", 280, 22, 7, 26, 4.8,
         {{"Куриное филе","200 гр"},{"Морковь","1 шт"},{"Лук","1 шт"},{"Картофель","2 шт"},{"Укроп","10 гр"},{"Соль, перец","по вкусу"}},
         {"Залить курицу 1.5 л воды, довести до кипения.","Снять пену, добавить целый лук и морковь.","Варить на слабом огне 30 мин.","Вынуть курицу, мясо разобрать и вернуть.","Добавить нарезанный картофель, варить 15 мин.","Посолить, поперчить, добавить укроп."} },

        { "Паста болоньезе", "Обед", "говядина", " ", 480, 28, 16, 55, 4.8,
         {{"Паста","150 гр"},{"Говяжий фарш","200 гр"},{"Томаты в/с","1 банка"},{"Лук","1 шт"},{"Чеснок","2 зубч."},{"Оливковое масло","1 ст.л"}},
         {"Отварить пасту аль-денте по инструкции.","Обжарить лук и чеснок на масле.","Добавить фарш, обжарить 10 мин.","Влить томаты, тушить 15 мин, приправить.","Смешать пасту с соусом и подавать."} },

        { "Куриные котлеты", "Обед", "курица", " ", 390, 42, 14, 18, 4.8,
         {{"Куриное филе","500 гр"},{"Яйцо","1 шт"},{"Лук","1 шт"},{"Хлеб","1 ломтик"},{"Соль, перец","по вкусу"}},
         {"Перекрутить курицу и лук через мясорубку.","Замочить хлеб в воде, отжать и добавить к фаршу.","Добавить яйцо, соль, перец — перемешать.","Сформировать котлеты и обвалять в панировке.","Обжарить по 5 мин с каждой стороны.","Довести до готовности в духовке 10 мин при 180°C."} },

        { "Лосось на гриле", "Ужин", "лосось", " ", 350, 40, 18, 4, 5.0,
         {{"Филе лосося","200 гр"},{"Лимон","½ шт"},{"Оливковое масло","1 чл"},{"Розмарин","1 веточка"},{"Соль, перец","по вкусу"}},
         {"Промыть и обсушить рыбу.","Смазать оливковым маслом, посолить и поперчить.","Добавить ломтики лимона и розмарин.","Жарить на гриле по 4 мин с каждой стороны.","Подавать с овощами или салатом."} },

        { "Творог с ягодами", "Ужин", "творог", "🍶", 190, 20, 4, 18, 4.7,
         {{"Творог 5%","200 гр"},{"Черника","50 гр"},{"Клубника","50 гр"},{"Мёд","1 чл"},{"Мята","по желанию"}},
         {"Выложить творог в тарелку.","Полить мёдом.","Украсить ягодами и листиками мяты.","Подавать сразу."} },

        { "Тушёные овощи", "Ужин", "кабачок", "🥦", 150, 5, 6, 20, 4.5,
         {{"Кабачок","1 шт"},{"Перец болгарский","1 шт"},{"Морковь","1 шт"},{"Чеснок","2 зубч."},{"Оливковое масло","1 ст.л"},{"Соль, специи","по вкусу"}},
         {"Нарезать все овощи средними кусочками.","Разогреть масло в сковороде.","Обжарить морковь 3 мин, добавить кабачок.","Добавить перец и чеснок, тушить 10 мин.","Приправить специями и подавать."} },

        { "Запечённая рыба", "Ужин", "треска", "🐟", 270, 35, 10, 6, 4.7,
         {{"Филе трески","200 гр"},{"Лимон","½ шт"},{"Чеснок","1 зубч."},{"Тимьян","1 чл"},{"Оливковое масло","1 чл"},{"Соль, перец","по вкусу"}},
         {"Разогреть духовку до 200°C.","Рыбу смазать маслом, приправить.","Добавить чеснок, тимьян, ломтики лимона.","Запекать 18–20 минут до готовности.","Подавать с зелёным салатом."} },

        { "Греческий салат", "Ужин", "огурец", "🥗", 220, 8, 16, 12, 4.7,
         {{"Огурец","1 шт"},{"Помидор","2 шт"},{"Маслины","50 гр"},{"Фета","60 гр"},{"Красный лук","¼ шт"},{"Оливковое масло","1 ст.л"}},
         {"Нарезать огурцы, помидоры и лук.","Смешать все овощи в миске.","Добавить маслины и покрошить фету.","Заправить оливковым маслом, посолить.","Подавать сразу."} },

        { "Протеиновый смузи", "Перекус", "банан", "🥤", 240, 28, 5, 22, 4.9,
         {{"Банан","1 шт"},{"Протеин ванильный","30 гр"},{"Молоко 1.5%","200 мл"},{"Лёд","по желанию"}},
         {"Положить все ингредиенты в блендер.","Взбить до однородной консистенции.","Добавить лёд при желании, ещё раз взбить.","Вылить в стакан и подавать сразу."} },

        { "Яблоко с арахисом", "Перекус", "яблоко", "🍎", 210, 7, 12, 24, 4.6,
         {{"Яблоко","1 шт"},{"Арахисовая паста","2 ст.л"},{"Корица","щепотка"}},
         {"Нарезать яблоко дольками.","Посыпать корицей.","Подавать с арахисовой пастой для макания."} },

        { "Рисовые блинчики", "Перекус", "рис", "🥞", 290, 12, 8, 40, 4.4,
         {{"Рисовая мука","60 гр"},{"Яйцо","1 шт"},{"Молоко","100 мл"},{"Мёд","1 чл"},{"Масло","½ чл"}},
         {"Смешать муку, яйцо и молоко до гладкого теста.","Добавить мёд, перемешать.","Жарить блинчики на смазанной сковороде 1–2 мин с каждой стороны.","Подавать с ягодами или мёдом."} },

        { "Энергетические шарики", "Перекус", "финики", "🍫", 310, 10, 14, 36, 4.5,
         {{"Финики без косточек","100 гр"},{"Овсяные хлопья","60 гр"},{"Арахисовая паста","2 ст.л"},{"Какао","1 ст.л"},{"Кокосовая стружка","для обвалки"}},
         {"Измельчить финики в блендере.","Смешать с хлопьями, пастой и какао.","Скатать шарики размером с грецкий орех.","Обвалять в кокосовой стружке.","Убрать в холодильник на 30 мин."} }
    };

    db.transaction();
    for (const auto &s : seeds) {
        // Вставить основную запись рецепта
        QSqlQuery ins(db);
        if (!ins.prepare("INSERT INTO Recipes (name, meal_type, kcal, protein, fat, carbs, rating, ingredient, img) "
                         "VALUES (:name,:mt,:kcal,:prot,:fat,:carbs,:rating,:ing,:img)")) {
            qWarning() << "seedInitialRecipes: prepare failed:" << ins.lastError().text();
            db.rollback();
            return;
        }
        ins.bindValue(":name",   s.name);
        ins.bindValue(":mt",     s.mealType);
        ins.bindValue(":kcal",   s.kcal);
        ins.bindValue(":prot",   s.protein);
        ins.bindValue(":fat",    s.fat);
        ins.bindValue(":carbs",  s.carbs);
        ins.bindValue(":rating", s.rating);
        ins.bindValue(":ing",    s.ingredient);
        ins.bindValue(":img",    QByteArray()); // пустой BLOB, фото добавляется отдельно

        if (!ins.exec()) {
            qWarning() << "seedInitialRecipes: insert failed for" << s.name << ins.lastError().text();
            continue;
        }
        int newId = ins.lastInsertId().toInt();

        // Ингредиенты
        for (const auto &pair : s.ingredients) {
            QSqlQuery iiq(db);
            iiq.prepare("INSERT INTO Recipe_Ingredients (recipe_id, ingredient_name, amount) VALUES (:rid,:ing,:amt)");
            iiq.bindValue(":rid", newId);
            iiq.bindValue(":ing", pair.first);
            iiq.bindValue(":amt", pair.second);
            iiq.exec();
        }

        // Шаги
        int order = 1;
        for (const auto &step : s.steps) {
            QSqlQuery stq(db);
            stq.prepare("INSERT INTO Recipe_Steps (recipe_id, step_order, step_text) VALUES (:rid,:ord,:txt)");
            stq.bindValue(":rid", newId);
            stq.bindValue(":ord", order++);
            stq.bindValue(":txt", step);
            stq.exec();
        }

        qDebug() << "Seeded recipe id=" << newId << s.name;
    }
    db.commit();
    qDebug() << "seedInitialRecipes: done," << seeds.size() << "recipes inserted.";
}

// ---------------------------------------------------------------------------
// Загрузить все рецепты → JSON-массив
// ---------------------------------------------------------------------------
bool UserManager::loadAllRecipes(QString &jsonData)
{
    QSqlQuery q(db);
    if (!q.exec("SELECT id, name, meal_type, kcal, protein, fat, carbs, rating, ingredient, img FROM Recipes ORDER BY id")) {
        qWarning() << "loadAllRecipes failed:" << q.lastError().text();
        return false;
    }

    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["recipeId"]   = q.value(0).toInt();
        obj["name"]       = q.value(1).toString();
        obj["mealType"]   = q.value(2).toString();
        obj["kcal"]       = q.value(3).toInt();
        obj["protein"]    = q.value(4).toInt();
        obj["fat"]        = q.value(5).toInt();
        obj["carbs"]      = q.value(6).toInt();
        obj["rating"]     = q.value(7).toDouble();
        obj["ingredient"] = q.value(8).toString();
        QByteArray imgBlob = q.value(9).toByteArray();
        obj["img"] = imgBlob.isEmpty() ? QString("") : QString::fromLatin1(imgBlob.toBase64());
        arr.append(obj);
    }

    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "loadAllRecipes: returned" << arr.size() << "recipes";
    return true;
}

// ---------------------------------------------------------------------------
// Загрузить детали рецепта (ингредиенты + шаги) → JSON-объект
// ---------------------------------------------------------------------------
bool UserManager::loadRecipeDetails(int recipe_id, QString &jsonData)
{
    // Ингредиенты
    QSqlQuery qi(db);
    qi.prepare("SELECT ingredient_name, amount FROM Recipe_Ingredients WHERE recipe_id = :id ORDER BY id");
    qi.bindValue(":id", recipe_id);
    if (!qi.exec()) {
        qWarning() << "loadRecipeDetails ingredients failed:" << qi.lastError().text();
        return false;
    }

    QJsonArray ingredients;
    while (qi.next()) {
        QJsonArray pair;
        pair.append(qi.value(0).toString());
        pair.append(qi.value(1).toString());
        ingredients.append(pair);
    }

    // Шаги
    QSqlQuery qs(db);
    qs.prepare("SELECT step_text FROM Recipe_Steps WHERE recipe_id = :id ORDER BY step_order");
    qs.bindValue(":id", recipe_id);
    if (!qs.exec()) {
        qWarning() << "loadRecipeDetails steps failed:" << qs.lastError().text();
        return false;
    }

    QJsonArray steps;
    while (qs.next()) {
        steps.append(qs.value(0).toString());
    }

    // Получить img (BLOB → base64)
    QSqlQuery qimg(db);
    qimg.prepare("SELECT img FROM Recipes WHERE id = :id");
    qimg.bindValue(":id", recipe_id);
    QByteArray imgBlob;
    if (qimg.exec() && qimg.next()) imgBlob = qimg.value(0).toByteArray();

    QJsonObject result;
    result["img"]         = imgBlob.isEmpty() ? QString("") : QString::fromLatin1(imgBlob.toBase64());
    result["ingredients"] = ingredients;
    result["steps"]       = steps;

    jsonData = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    qDebug() << "loadRecipeDetails: recipe_id=" << recipe_id << "ingr=" << ingredients.size() << "steps=" << steps.size();
    return true;
}

// ---------------------------------------------------------------------------
// Добавить новый рецепт
// ---------------------------------------------------------------------------
bool UserManager::addRecipe(const QString &name,
                            const QString &mealType,
                            int kcal, int protein, int fat, int carbs,
                            double rating,
                            const QString &ingredient,
                            const QString &img,
                            const QString &ingredientsJson,
                            const QString &stepsJson,
                            int &newId)
{
    // Парсим ингредиенты [[name,amount],...]
    QJsonArray ingArr  = QJsonDocument::fromJson(ingredientsJson.toUtf8()).array();
    QJsonArray stepArr = QJsonDocument::fromJson(stepsJson.toUtf8()).array();

    db.transaction();

    QSqlQuery ins(db);
    if (!ins.prepare("INSERT INTO Recipes (name, meal_type, kcal, protein, fat, carbs, rating, ingredient, img) "
                     "VALUES (:name,:mt,:kcal,:prot,:fat,:carbs,:rating,:ing,:img)")) {
        db.rollback();
        qWarning() << "addRecipe prepare failed:" << ins.lastError().text();
        return false;
    }
    ins.bindValue(":name",   name);
    ins.bindValue(":mt",     mealType);
    ins.bindValue(":kcal",   kcal);
    ins.bindValue(":prot",   protein);
    ins.bindValue(":fat",    fat);
    ins.bindValue(":carbs",  carbs);
    ins.bindValue(":rating", rating);
    ins.bindValue(":ing",    ingredient);
    // img приходит как base64-строка → декодируем в BLOB; пустая → пустой массив
    ins.bindValue(":img",    img.isEmpty() ? QByteArray() : QByteArray::fromBase64(img.toLatin1()));

    if (!ins.exec()) {
        db.rollback();
        qWarning() << "addRecipe insert failed:" << ins.lastError().text();
        return false;
    }
    newId = ins.lastInsertId().toInt();

    // Ингредиенты
    for (const QJsonValue &v : ingArr) {
        QJsonArray pair = v.toArray();
        if (pair.size() < 2) continue;
        QSqlQuery iiq(db);
        iiq.prepare("INSERT INTO Recipe_Ingredients (recipe_id, ingredient_name, amount) VALUES (:rid,:ing,:amt)");
        iiq.bindValue(":rid", newId);
        iiq.bindValue(":ing", pair[0].toString());
        iiq.bindValue(":amt", pair[1].toString());
        if (!iiq.exec()) {
            db.rollback();
            qWarning() << "addRecipe ingredient insert failed:" << iiq.lastError().text();
            return false;
        }
    }

    // Шаги
    int order = 1;
    for (const QJsonValue &v : stepArr) {
        QSqlQuery stq(db);
        stq.prepare("INSERT INTO Recipe_Steps (recipe_id, step_order, step_text) VALUES (:rid,:ord,:txt)");
        stq.bindValue(":rid", newId);
        stq.bindValue(":ord", order++);
        stq.bindValue(":txt", v.toString());
        if (!stq.exec()) {
            db.rollback();
            qWarning() << "addRecipe step insert failed:" << stq.lastError().text();
            return false;
        }
    }

    db.commit();
    qDebug() << "addRecipe: new recipe id=" << newId << name;
    return true;
}

//Хэширование пароля (SHA-256 + соль)
void UserManager::hashPassword(QString &password)
{
    // ⚠️ LEGACY: одна общая соль для всех. Используется только для проверки
    // паролей пользователей, зарегистрированных до миграции на per-user salt.
    static const QByteArray salt = QByteArrayLiteral("KachFitness_v1_salt");
    QByteArray data = salt + password.toUtf8();
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    password = QString::fromLatin1(hash.toHex());
}

// ✅ Хеш с per-user солью + 100k итераций PBKDF2-подобный stretching через цикл SHA-256
// (для устойчивости к brute-force даже при утечке БД)
QString UserManager::hashPasswordWithSalt(const QString &password, const QByteArray &salt) const
{
    QByteArray data = salt + password.toUtf8();
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    // Растяжение: 100 000 итераций SHA-256 для замедления brute-force
    for (int i = 0; i < 100000; ++i) {
        hash = QCryptographicHash::hash(hash + salt, QCryptographicHash::Sha256);
    }
    return QString::fromLatin1(hash.toHex());
}

// ✅ Криптографически случайная соль 16 байт (=32 hex-символа)
QByteArray UserManager::generateSalt() const
{
    QByteArray salt;
    salt.reserve(16);
    for (int i = 0; i < 16; ++i) {
        salt.append(static_cast<char>(QRandomGenerator::system()->bounded(0, 256)));
    }
    return salt.toHex();   // храним как hex-строку
}

//Функция регистрации пользователся
bool UserManager::registerUser(const QString &firstName, const QString &lastName, const QString &email, const QString &passwordHash, const QString &gender, const QString &birthDate, double height, double weight, QString &errorMessage, int& userId)
{
    // ✅ Валидация входных данных (защита от пустых полей и аномалий)
    if (email.trimmed().isEmpty() || !email.contains('@')) {
        errorMessage = "Некорректный email";
        return false;
    }
    if (passwordHash.size() < 6) {
        errorMessage = "Пароль слишком короткий (минимум 6 символов)";
        return false;
    }
    if (firstName.trimmed().isEmpty() || lastName.trimmed().isEmpty()) {
        errorMessage = "Имя и фамилия обязательны";
        return false;
    }
    if (height <= 0 || height > 300 || weight <= 0 || weight > 500) {
        errorMessage = "Некорректные параметры роста/веса";
        return false;
    }

    QSqlQuery query(db);

    // ✅ Генерируем персональную соль и хешируем пароль с ней
    QByteArray salt = generateSalt();
    QString hashed = hashPasswordWithSalt(passwordHash, salt);

    //Запрос на вставку данных
    query.prepare(
        "INSERT INTO USERS "
        "(first_name, last_name, email, password_hash, password_salt, gender, birth_date, height, weight) "
        "VALUES (:first_name, :last_name, :email, :password_hash, :password_salt, :gender, :birth_date, :height, :weight)");

    //Параметры соответствуют порядку полей
    query.bindValue(":first_name", firstName);
    query.bindValue(":last_name", lastName);
    query.bindValue(":email", email);
    query.bindValue(":password_hash", hashed);
    query.bindValue(":password_salt", QString::fromLatin1(salt));
    query.bindValue(":gender", gender);
    query.bindValue(":birth_date", birthDate);
    query.bindValue(":height", height);
    query.bindValue(":weight", weight);

    if (query.exec())
    {
        //Получаем ID вновь созданного пользователя
        userId = query.lastInsertId().toInt();

        // Генерируем уникальный 6-значный userFriendID
        int friendId = 0;
        for (int attempt = 0; attempt < 200; ++attempt) {
            int candidate = static_cast<int>(QRandomGenerator::global()->bounded(100000u, 1000000u));
            QSqlQuery chk(db);
            chk.prepare("SELECT 1 FROM USERS WHERE userFriendID = :fid");
            chk.bindValue(":fid", candidate);
            if (chk.exec() && !chk.next()) {
                friendId = candidate;
                break;
            }
        }
        if (friendId != 0) {
            QSqlQuery upd(db);
            upd.prepare("UPDATE USERS SET userFriendID = :fid WHERE user_id = :uid");
            upd.bindValue(":fid", friendId);
            upd.bindValue(":uid", userId);
            if (!upd.exec())
                qWarning() << "Failed to set userFriendID:" << upd.lastError().text();
            else
                qDebug() << "Generated userFriendID:" << friendId << "for user:" << userId;
        } else {
            qWarning() << "Could not generate unique friendId for user:" << userId;
        }

        return true;
    }
    else
    {
        //Сообщение об ошибки
        errorMessage = query.lastError().text();

        return false;
    }
}

//Проверка учетных данных
bool UserManager::authenticateUser(const QString &email, const QString &password, int& userId)
{
    // ✅ Базовая валидация
    if (email.trimmed().isEmpty() || password.isEmpty())
        return false;

    QSqlQuery query(db);

    // Получаем сохранённый пароль + соль по email
    query.prepare("SELECT user_id, password_hash, COALESCE(password_salt, '') FROM users WHERE email=:email");
    query.bindValue(":email", email);

    if (!query.exec() || !query.next())
        return false;

    const int foundId = query.value(0).toInt();
    const QString stored = query.value(1).toString();
    const QString saltStored = query.value(2).toString();

    // ✅ Новый формат: per-user соль + 100k итераций
    if (!saltStored.isEmpty()) {
        const QString newHash = hashPasswordWithSalt(password, saltStored.toLatin1());
        if (stored.compare(newHash, Qt::CaseInsensitive) == 0) {
            userId = foundId;
            return true;
        }
        return false;   // соль есть, но не совпадает — отказ
    }

    // ⚠️ LEGACY: пользователь зарегистрирован до миграции (статическая соль)
    QString hashedOld = password;
    hashPassword(hashedOld);

    if (stored.compare(hashedOld, Qt::CaseInsensitive) == 0) {
        // ✅ Миграция на новый формат: генерируем соль и пересохраняем
        const QByteArray newSalt = generateSalt();
        const QString newHash = hashPasswordWithSalt(password, newSalt);
        QSqlQuery upd(db);
        upd.prepare("UPDATE users SET password_hash=:h, password_salt=:s WHERE user_id=:uid");
        upd.bindValue(":h", newHash);
        upd.bindValue(":s", QString::fromLatin1(newSalt));
        upd.bindValue(":uid", foundId);
        if (!upd.exec())
            qWarning() << "Не удалось мигрировать пароль на новый формат:" << upd.lastError().text();
        else
            qDebug() << "✅ Пароль user_id=" << foundId << "мигрирован на per-user salt";
        userId = foundId;
        return true;
    }

    // ⚠️ legacy plaintext fallback УДАЛЁН — это была уязвимость
    return false;
}

//Загрузка данных из бд
bool UserManager::loadUserData(int user_id, QString &first_name, QString &last_name, QString &gender, QString &Age, double &height, double &weight, QString &Goal, int &PlanTrainninnUserData, int &standardSubscription, int &vipSubscription, int &subscriptionFrozen, int &feedbackSubscription)
{
    QSqlQuery query(db);
    query.prepare("SELECT first_name, last_name, gender, birth_date, height, weight, Goal, PlanTrainning, "
                  "StandardSubscription, COALESCE(VipSubscription, 0), COALESCE(subscription_frozen, 0), "
                  "COALESCE(FeedbackSubscription, 0) "
                  "FROM users WHERE user_id=:user_id;");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        first_name            = query.value(0).toString();
        last_name             = query.value(1).toString();
        gender                = query.value(2).toString();
        Age                   = query.value(3).toString();
        height                = query.value(4).toDouble();
        weight                = query.value(5).toDouble();
        Goal                  = query.value(6).toString();
        PlanTrainninnUserData  = query.value(7).toInt();
        standardSubscription  = query.value(8).toInt();
        vipSubscription       = query.value(9).toInt();
        subscriptionFrozen    = query.value(10).toInt();
        feedbackSubscription  = query.value(11).toInt();

        // ✅ Если подписка заморожена — обнуляем ВСЕ статусы доступа для клиента.
        if (subscriptionFrozen) {
            standardSubscription = 0;
            vipSubscription = 0;
            feedbackSubscription = 0;
        }
        return true;
    }

    return false;
}

//Обновление данных в БД
bool UserManager::updateUserData(int user_id, QString &first_name, QString &last_name, QString &gender, QString &Age, double &height, double &weight, QString &Goal)
{
    //Объект типа запроса
    QSqlQuery query(db);

    // Запрос на обновление данных в базе данных
    query.prepare(
        "UPDATE users "
        "SET first_name=:first_name, last_name=:last_name, gender=:gender, birth_date=:birth_date, height=:height, weight=:weight, Goal=:Goal "
        "WHERE user_id=:user_id;"
        );

    // Привязываем значения
    query.bindValue(":user_id", user_id);
    query.bindValue(":first_name", first_name);
    query.bindValue(":last_name", last_name);
    query.bindValue(":gender", gender);
    query.bindValue(":birth_date", Age);
    query.bindValue(":height", height);
    query.bindValue(":weight", weight);
    query.bindValue(":Goal", Goal);

    if (!query.exec())
    {
        //Выводим сообщение об ошибке
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    return true;
}

bool UserManager::saveImage(int user_id, QByteArray &image)
{
    // Готовим SQL-запрос для вставки изображения
    QSqlQuery query(db);

    // Исправлена команда UPDATE
    query.prepare("UPDATE users SET imageAvatar=:imageAvatar WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    query.bindValue(":imageAvatar", image);

    // Выполнение запроса
    if(query.exec())
    {
        qDebug() << "Image for user" << user_id << "saved successfully.";
        return true;
    }
    else
    {
        qCritical() << "Error saving image:" << query.lastError().text();
        return false;
    }
}

bool UserManager::getImage(int user_id, QByteArray &imageData)
{
    // Готовим SQL-запрос для выборки изображения
    QSqlQuery query(db);

    query.prepare("SELECT imageAvatar FROM users WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        // Сообщаем об ошибке
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    // Проверяем наличие результата
    if (query.next())
    {
        // Приводим результат к типу QByteArray
        imageData = query.value(0).toByteArray();

        return true;
    }

    return false;
}

bool UserManager::saveEPlanTrainningUserData(int user_id, int &EPlanTrainningUser)
{
    // Сначала узнаем текущий план, чтобы понять — это смена/очистка или нет
    int oldPlan = -1;
    {
        QSqlQuery qCur(db);
        qCur.prepare("SELECT PlanTrainning FROM users WHERE user_id=:user_id");
        qCur.bindValue(":user_id", user_id);
        if (qCur.exec() && qCur.next())
            oldPlan = qCur.value(0).toInt();
    }

    QSqlQuery query(db);
    query.prepare("UPDATE users SET PlanTrainning=:PlanTrainning WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    query.bindValue(":PlanTrainning", EPlanTrainningUser);

    if(!query.exec())
    {
        qCritical() << "Error saving PlanTrainning:" << query.lastError().text();
        return false;
    }

    qDebug() << "PlanTrainning for user" << user_id << "saved successfully. old=" << oldPlan
             << "new=" << EPlanTrainningUser;

    // Если план реально изменился (включая очистку в 0 и выбор нового после завершения) —
    // полностью обнуляем статистику тренировок и прогресс недель: подходы, вес — всё в ноль,
    // как будто пользователь начал с самого начала.
    if (oldPlan != EPlanTrainningUser) {
        // 1. Стираем недельную статистику (подходы, поднятый вес)
        clearWorkoutStats(user_id);

        // 2. Сбрасываем прогресс тренировочных недель
        QSqlQuery qReset(db);
        qReset.prepare("UPDATE Training_Week_Progress SET "
                       "current_week = 1, "
                       "workout1_done = 0, workout2_done = 0, workout3_done = 0, "
                       "w1_date = '', w2_date = '', w3_date = '', "
                       "week_start_date = '', "
                       "week_completed = 0 "
                       "WHERE user_id = :uid");
        qReset.bindValue(":uid", user_id);
        if (!qReset.exec()) {
            qWarning() << "saveEPlanTrainningUserData: reset week progress error:"
                       << qReset.lastError().text();
        }

        qDebug() << "✅ Stats & week progress wiped for user" << user_id
                 << "(plan changed" << oldPlan << "->" << EPlanTrainningUser << ")";
    }

    return true;
}

bool UserManager::savePlanStartDate(int user_id, const QString &datetime)
{
    QSqlQuery q(db);
    q.prepare("INSERT INTO Training_Week_Progress (user_id, plan_start_date) "
              "VALUES (:uid, :dt) "
              "ON CONFLICT(user_id) DO UPDATE SET plan_start_date=:dt");
    q.bindValue(":uid", user_id);
    q.bindValue(":dt", datetime);
    if (q.exec()) {
        qDebug() << "plan_start_date saved for user" << user_id << ":" << datetime;
        return true;
    }
    qCritical() << "Error saving plan_start_date:" << q.lastError().text();
    return false;
}

bool UserManager::getPlanStartDate(int user_id, QString &datetime)
{
    QSqlQuery q(db);
    q.prepare("SELECT plan_start_date FROM Training_Week_Progress WHERE user_id=:uid");
    q.bindValue(":uid", user_id);
    if (q.exec() && q.next()) {
        datetime = q.value(0).toString();
        return true;
    }
    datetime.clear();
    return false;
}

bool UserManager::isPlanChangeDue(int user_id, bool &due)
{
    QString planStartStr;
    if (!getPlanStartDate(user_id, planStartStr) || planStartStr.isEmpty()) {
        due = false;
        return false;
    }

    QDateTime planStart = QDateTime::fromString(planStartStr, "yyyy-MM-dd hh:mm:ss");
    if (!planStart.isValid()) {
        due = false;
        return false;
    }

#ifdef TEST_WEEK_ADVANCE
    // Тест: 2 минуты = 1 неделя → 2 месяца ≈ 8 недель = 16 минут
    due = planStart.secsTo(QDateTime::currentDateTime()) >= 16 * 60;
    qDebug() << "🧪 isPlanChangeDue TEST: elapsed secs="
             << planStart.secsTo(QDateTime::currentDateTime())
             << "due=" << due;
#else
    due = planStart.daysTo(QDateTime::currentDateTime()) >= 60;
    qDebug() << "isPlanChangeDue: elapsed days="
             << planStart.daysTo(QDateTime::currentDateTime())
             << "due=" << due;
#endif
    return true;
}

bool UserManager::isPlanResetDue(int user_id, bool &due)
{
    QString planStartStr;
    if (!getPlanStartDate(user_id, planStartStr) || planStartStr.isEmpty()) {
        due = false;
        return false;
    }

    QDateTime planStart = QDateTime::fromString(planStartStr, "yyyy-MM-dd hh:mm:ss");
    if (!planStart.isValid()) {
        due = false;
        return false;
    }

#ifdef TEST_WEEK_ADVANCE
    // Тест: 2 минуты = 1 неделя → 3 месяца ≈ 12 недель = 24 минуты
    due = planStart.secsTo(QDateTime::currentDateTime()) >= 24 * 60;
#else
    // 3 месяца = 90 дней
    due = planStart.daysTo(QDateTime::currentDateTime()) >= 90;
#endif
    return true;
}

bool UserManager::saveUserMeasurements(int &user_id, QString &Data, QString &Weight, QString &Chest_Circumference, QString &Waist_Circumference, QString &Hip_Circumference, QString &Total_Steps_Day)
{
    // Готовим SQL-запрос для вставки замеров
    QSqlQuery query(db);

    query.prepare("INSERT INTO UserMeasurements "
                  "(UserId, Data, Weight, Chest_Circumference, Waist_Circumference, Hip_Circumference, Total_Steps_Day) "
                  "VALUES (:UserId, :Data, :Weight, :Chest_Circumference, :Waist_Circumference, :Hip_Circumference, :Total_Steps_Day)");

    //Параметры соответствуют порядку полей
    query.bindValue(":UserId", user_id);
    query.bindValue(":Data", Data);
    query.bindValue(":Weight", Weight);
    query.bindValue(":Chest_Circumference", Chest_Circumference);
    query.bindValue(":Waist_Circumference", Waist_Circumference);
    query.bindValue(":Hip_Circumference", Hip_Circumference);
    query.bindValue(":Total_Steps_Day", Total_Steps_Day);

    if(query.exec())
    {
        qDebug() << "UserMeasurements for user" << user_id << "saved successfully.";
        return true;
    }
    else
    {
        qDebug() << "UserMeasurements for user" << user_id << "saved ERROR: " << query.lastError().text();
        return false;
    }
}

bool UserManager::createMeasurement(int user_id, const QString &date, const QString &weight,
                                    const QString &chest, const QString &waist, const QString &hips,
                                    const QString &steps, int &measurement_id)
{
    qDebug() << "=== createMeasurement ===" << "User:" << user_id << "Date:" << date;

    QSqlQuery query(db);

    query.prepare("INSERT INTO UserMeasurements "
                  "(UserId, Data, Weight, Chest_Circumference, Waist_Circumference, Hip_Circumference, Total_Steps_Day) "
                  "VALUES (:UserId, :Data, :Weight, :Chest_Circumference, :Waist_Circumference, :Hip_Circumference, :Total_Steps_Day)");

    query.bindValue(":UserId", user_id);
    query.bindValue(":Data", date);
    query.bindValue(":Weight", weight.toDouble());
    query.bindValue(":Chest_Circumference", chest.toDouble());
    query.bindValue(":Waist_Circumference", waist.toDouble());
    query.bindValue(":Hip_Circumference", hips.toDouble());
    query.bindValue(":Total_Steps_Day", steps.toInt());

    if(query.exec())
    {
        measurement_id = query.lastInsertId().toInt();
        qDebug() << "Measurement created for user" << user_id << "ID:" << measurement_id;
        return true;
    }
    else
    {
        qDebug() << "Error creating measurement:" << query.lastError().text();
        return false;
    }
}

bool UserManager::saveMeasurementPhoto(int user_id, int measurement_id, const QString &photoType, const QByteArray &photoData)
{
    qDebug() << "=== saveMeasurementPhoto ===" << "User:" << user_id << "Measurement:" << measurement_id << "Type:" << photoType;

    QSqlQuery query(db);

    QString columnName;
    if(photoType == "front") {
        columnName = "Front_Photo";
    } else if(photoType == "side") {
        columnName = "Side_Photo";
    } else if(photoType == "back") {
        columnName = "Photo_Behind";
    } else {
        qDebug() << "Invalid photo type:" << photoType;
        return false;
    }

    // Исправлен синтаксис - используем id вместо measurement_id
    QString queryStr = QString("UPDATE UserMeasurements SET %1 = :photoData WHERE id = :measurement_id").arg(columnName);
    query.prepare(queryStr);

    query.bindValue(":measurement_id", measurement_id);
    query.bindValue(":photoData", photoData);

    if(query.exec())
    {
        qDebug() << "Photo saved successfully for measurement" << measurement_id << "type:" << photoType;
        return true;
    }
    else
    {
        qDebug() << "Error saving photo:" << query.lastError().text();
        return false;
    }
}

bool UserManager::getMeasurementPhoto(int measurement_id, const QString &photoType, QByteArray &photoData)
{
    qDebug() << "=== getMeasurementPhoto ===" << "Measurement:" << measurement_id << "Type:" << photoType;

    QSqlQuery query(db);

    QString columnName;
    if(photoType == "front") {
        columnName = "Front_Photo";
    } else if(photoType == "side") {
        columnName = "Side_Photo";
    } else if(photoType == "back") {
        columnName = "Photo_Behind";
    } else {
        qDebug() << "Invalid photo type:" << photoType;
        return false;
    }

    // Исправлен синтаксис - используем id вместо measurement_id
    QString queryStr = QString("SELECT %1 FROM UserMeasurements WHERE id = :measurement_id").arg(columnName);
    query.prepare(queryStr);
    query.bindValue(":measurement_id", measurement_id);

    if(!query.exec())
    {
        qDebug() << "Error executing query:" << query.lastError().text();
        return false;
    }

    if(query.next())
    {
        photoData = query.value(0).toByteArray();
        qDebug() << "Photo retrieved successfully, size:" << photoData.size();
        return true;
    }

    qDebug() << "No photo found for measurement" << measurement_id;
    return false;
}

bool UserManager::loadUserMeasurements(int user_id, QString &jsonData)
{
    qDebug() << "=== loadUserMeasurements ===" << "User:" << user_id;

    QSqlQuery query(db);

    // SELECT с COALESCE: новые записи лежат в Chest_Circumference, старые
    // (до миграции) — в Neck_Circumference. Берём ту, что не NULL.
    query.prepare("SELECT id, Data, Weight, "
                  "COALESCE(Chest_Circumference, Neck_Circumference, 0), "
                  "Waist_Circumference, Hip_Circumference, Total_Steps_Day "
                  "FROM UserMeasurements WHERE UserId = :UserId ORDER BY Data DESC");
    query.bindValue(":UserId", user_id);

    if(!query.exec())
    {
        qDebug() << "Error loading measurements:" << query.lastError().text();
        return false;
    }

    QJsonArray array;
    while(query.next())
    {
        QJsonObject obj;
        obj["id"] = query.value(0).toInt();
        obj["date"] = query.value(1).toString();
        obj["weight"] = query.value(2).toDouble();
        obj["chestCircumference"] = query.value(3).toDouble();
        obj["waistCircumference"] = query.value(4).toDouble();
        obj["hipCircumference"] = query.value(5).toDouble();
        obj["steps"] = query.value(6).toInt();

        // Фото загружаются отдельно если нужны
        obj["frontPhoto"] = "";
        obj["sidePhoto"] = "";
        obj["backPhoto"] = "";

        array.append(obj);
    }

    QJsonDocument doc(array);
    jsonData = QString::fromUtf8(doc.toJson());

    qDebug() << "Loaded" << array.size() << "measurements";
    return true;
}

// ✅ СОХРАНЕНИЕ ТРЕНИРОВОК

bool UserManager::saveTrainingButtockBridgeP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingButtockBridgeP1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    // 1. Проверяем наличие записи
    query.prepare("SELECT user_id FROM Save_Training_Buttock_Bridge_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    // 2. Если запись существует - UPDATE
    if (query.next())
    {
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Buttock_Bridge_P1 "
                            "SET Approach1=:Approach1, "
                            "    Approach2=:Approach2, "
                            "    Approach3=:Approach3 "
                            "WHERE user_id=:user_id");

        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec())
        {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }

        qDebug() << "Запись обновлена для пользователя:" << user_id;
        return true;
    }

    // 3. Если записи нет - INSERT
    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Buttock_Bridge_P1 "
                        "(user_id, Approach1, Approach2, Approach3) "
                        "VALUES (:user_id, :Approach1, :Approach2, :Approach3)");

    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec())
    {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }

    qDebug() << "Новая запись создана для пользователя:" << user_id;
    return true;
}

bool UserManager::saveTrainingChestPressP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== SaveTrainingChestPressP1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    query.prepare("SELECT user_id FROM Save_Training_Chest_Press_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Chest_Press_P1 "
                            "SET Approach1=:Approach1, "
                            "    Approach2=:Approach2, "
                            "    Approach3=:Approach3 "
                            "WHERE user_id=:user_id");

        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec())
        {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }

        qDebug() << "Запись обновлена для пользователя:" << user_id;
        return true;
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Chest_Press_P1 "
                        "(user_id, Approach1, Approach2, Approach3) "
                        "VALUES (:user_id, :Approach1, :Approach2, :Approach3)");

    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec())
    {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }

    qDebug() << "Новая запись создана для пользователя:" << user_id;
    return true;
}

bool UserManager::saveTrainingHorizontalThrustP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingHorizontalThrust_P1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    query.prepare("SELECT user_id FROM Save_Training_Horizontal_Thrust_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Horizontal_Thrust_P1 "
                            "SET Approach1=:Approach1, "
                            "    Approach2=:Approach2, "
                            "    Approach3=:Approach3 "
                            "WHERE user_id=:user_id");

        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec())
        {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }

        qDebug() << "Запись обновлена для пользователя:" << user_id;
        return true;
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Horizontal_Thrust_P1 "
                        "(user_id, Approach1, Approach2, Approach3) "
                        "VALUES (:user_id, :Approach1, :Approach2, :Approach3)");

    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec())
    {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }

    qDebug() << "Новая запись создана для по��ьзователя:" << user_id;
    return true;
}

bool UserManager::saveTrainingOneLegBenchPressP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== Save_Training_One_Leg_Bench_Press_P1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    query.prepare("SELECT user_id FROM Save_Training_One_Leg_Bench_Press_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_One_Leg_Bench_Press_P1 "
                            "SET Approach1=:Approach1, "
                            "    Approach2=:Approach2, "
                            "    Approach3=:Approach3 "
                            "WHERE user_id=:user_id");

        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec())
        {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }

        qDebug() << "Запись обновлена для пользователя:" << user_id;
        return true;
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_One_Leg_Bench_Press_P1 "
                        "(user_id, Approach1, Approach2, Approach3) "
                        "VALUES (:user_id, :Approach1, :Approach2, :Approach3)");

    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec())
    {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }

    qDebug() << "Новая запись создана для пользователя:" << user_id;
    return true;
}

bool UserManager::saveTrainingPressOnMatP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== Save_Training_Press_On_Mat_P1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    query.prepare("SELECT user_id FROM Save_Training_Press_On_Mat_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Press_On_Mat_P1 "
                            "SET Approach1=:Approach1, "
                            "    Approach2=:Approach2, "
                            "    Approach3=:Approach3 "
                            "WHERE user_id=:user_id");

        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec())
        {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }

        qDebug() << "Запись обновлена для пользователя:" << user_id;
        return true;
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Press_On_Mat_P1 "
                        "(user_id, Approach1, Approach2, Approach3) "
                        "VALUES (:user_id, :Approach1, :Approach2, :Approach3)");

    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec())
    {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }

    qDebug() << "Новая запись создана для пользователя:" << user_id;
    return true;
}

bool UserManager::saveTrainingVerticalThrustP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== Save_Training_Vertical_Thrust_P1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    query.prepare("SELECT user_id FROM Save_Training_Vertical_Thrust_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Vertical_Thrust_P1 "
                            "SET Approach1=:Approach1, "
                            "    Approach2=:Approach2, "
                            "    Approach3=:Approach3 "
                            "WHERE user_id=:user_id");

        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec())
        {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }

        qDebug() << "Запись обновлена для пользователя:" << user_id;
        return true;
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Vertical_Thrust_P1 "
                        "(user_id, Approach1, Approach2, Approach3) "
                        "VALUES (:user_id, :Approach1, :Approach2, :Approach3)");

    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec())
    {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }

    qDebug() << "Новая запись создана для пользователя:" << user_id;
    return true;
}

// ✅ ЗАГРУЗКА ТРЕНИРОВОК

bool UserManager::GetSaveTrainingButtockBridgeP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingButtockBridgeP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Buttock_Bridge_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();

        qDebug() << "Данные получены:" << approach1 << approach2 << approach3;
        return true;
    }

    qDebug() << "Данные не найдены для пользователя:" << user_id;
    return false;
}

bool UserManager::GetSaveTrainingChestPressP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingChestPressP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Chest_Press_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();

        qDebug() << "Данные получены:" << approach1 << approach2 << approach3;
        return true;
    }

    qDebug() << "Данные не найдены для пользователя:" << user_id;
    return false;
}

bool UserManager::GetSaveTrainingHorizontalThrustP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingHorizontalThrustP1 ===" << "User:" << user_id;

    QSqlQuery query(db);

    // ✅ ИСПРАВЛЕНО - убрана точка с запятой в конце SQL запроса
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Horizontal_Thrust_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();

        qDebug() << "Данные найдены:" << approach1 << approach2 << approach3;
        return true;
    }

    qDebug() << "Данные не найдены для пользователя:" << user_id;
    return false;
}

bool UserManager::GetSaveTrainingOneLegBenchPressP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingOneLegBenchPressP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_One_Leg_Bench_Press_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();

        qDebug() << "Данные получены:" << approach1 << approach2 << approach3;
        return true;
    }

    qDebug() << "Данные не найдены для пользователя:" << user_id;
    return false;
}

bool UserManager::GetSaveTrainingPressOnMatP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingPressOnMatP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Press_On_Mat_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();

        qDebug() << "Данные получены:" << approach1 << approach2 << approach3;
        return true;
    }

    qDebug() << "Данные не найдены для пользователя:" << user_id;
    return false;
}

bool UserManager::GetSaveTrainingVerticalThrustP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingVerticalThrustP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Vertical_Thrust_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec())
    {
        qWarning() << "Ошибка выполнения запроса:" << query.lastError().text();
        return false;
    }

    if (query.next())
    {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();

        qDebug() << "Данные получены:" << approach1 << approach2 << approach3;
        return true;
    }

    qDebug() << "Данные не найдены для пользователя:" << user_id;
    return false;
}

// ✅ SAVE TRAINING - PRESS_ON_BALL_P1
bool UserManager::saveTrainingPressOnBallP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingPressOnBallP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Press_On_Ball_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) {
        qWarning() << "Ошибка выполнения запроса SELECT:" << query.lastError().text();
        return false;
    }

    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Press_On_Ball_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);

        if (!updateQuery.exec()) {
            qWarning() << "Ошибка обновления записи:" << updateQuery.lastError().text();
            return false;
        }
        return true;
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Press_On_Ball_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);

    if (!insertQuery.exec()) {
        qWarning() << "Ошибка вставки записи:" << insertQuery.lastError().text();
        return false;
    }
    return true;
}

// ✅ SAVE TRAINING - KETTLEBELL_SQUAT_P1
bool UserManager::saveTrainingKettlebellSquatP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingKettlebellSquatP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Kettlebell_Squat_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Kettlebell_Squat_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Kettlebell_Squat_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - ROMANIAN_DEADLIFT_P1
bool UserManager::saveTrainingRomanianDeadliftP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingRomanianDeadliftP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Romanian_Deadlift_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Romanian_Deadlift_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Romanian_Deadlift_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - KNEE_PUSHUP_P1
bool UserManager::saveTrainingKneePushupP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingKneePushupP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Knee_Pushup_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Knee_Pushup_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Knee_Pushup_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - HIP_ABDUCTION_P1
bool UserManager::saveTrainingHipAbductionP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingHipAbductionP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Hip_Abduction_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Hip_Abduction_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Hip_Abduction_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - TRICEP_EXTENSION_P1
bool UserManager::saveTrainingTricepExtensionP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingTricepExtensionP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Tricep_Extension_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Tricep_Extension_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Tricep_Extension_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - ASSISTED_PULLUP_P1
bool UserManager::saveTrainingAssistedPullupP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingAssistedPullupP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Assisted_Pullup_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Assisted_Pullup_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Assisted_Pullup_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - BULGARIAN_SPLIT_SQUAT_P1
bool UserManager::saveTrainingBulgarianSplitSquatP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingBulgarianSplitSquatP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Bulgarian_Split_Squat_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Bulgarian_Split_Squat_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Bulgarian_Split_Squat_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - SHOULDER_PRESS_P1
bool UserManager::saveTrainingShoulderPressP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingShoulderPressP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Shoulder_Press_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Shoulder_Press_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Shoulder_Press_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - CABLE_FLY_P1
bool UserManager::saveTrainingCableFlyP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingCableFlyP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Cable_Fly_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Cable_Fly_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Cable_Fly_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - LEG_EXTENSION_P1
bool UserManager::saveTrainingLegExtensionP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingLegExtensionP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Leg_Extension_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Leg_Extension_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Leg_Extension_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - PLANK_P1
bool UserManager::saveTrainingPlankP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingPlankP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Plank_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Plank_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Plank_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ SAVE TRAINING - CHEST_FLY_P1
bool UserManager::saveTrainingChestFlyP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingChestFlyP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Chest_Fly_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        query.finish(); // Закрываем SELECT перед UPDATE
        QSqlQuery updateQuery(db);
        updateQuery.prepare("UPDATE Save_Training_Chest_Fly_P1 SET Approach1=:Approach1, Approach2=:Approach2, Approach3=:Approach3 WHERE user_id=:user_id");
        updateQuery.bindValue(":Approach1", approach1);
        updateQuery.bindValue(":Approach2", approach2);
        updateQuery.bindValue(":Approach3", approach3);
        updateQuery.bindValue(":user_id", user_id);
        return updateQuery.exec();
    }

    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO Save_Training_Chest_Fly_P1 (user_id, Approach1, Approach2, Approach3) VALUES (:user_id, :Approach1, :Approach2, :Approach3)");
    insertQuery.bindValue(":user_id", user_id);
    insertQuery.bindValue(":Approach1", approach1);
    insertQuery.bindValue(":Approach2", approach2);
    insertQuery.bindValue(":Approach3", approach3);
    return insertQuery.exec();
}

// ✅ GET TRAINING - PRESS_ON_BALL_P1
bool UserManager::GetSaveTrainingPressOnBallP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingPressOnBallP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Press_On_Ball_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - KETTLEBELL_SQUAT_P1
bool UserManager::GetSaveTrainingKettlebellSquatP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingKettlebellSquatP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Kettlebell_Squat_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - ROMANIAN_DEADLIFT_P1
bool UserManager::GetSaveTrainingRomanianDeadliftP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingRomanianDeadliftP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Romanian_Deadlift_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - KNEE_PUSHUP_P1
bool UserManager::GetSaveTrainingKneePushupP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingKneePushupP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Knee_Pushup_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - HIP_ABDUCTION_P1
bool UserManager::GetSaveTrainingHipAbductionP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingHipAbductionP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Hip_Abduction_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - TRICEP_EXTENSION_P1
bool UserManager::GetSaveTrainingTricepExtensionP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingTricepExtensionP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Tricep_Extension_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - ASSISTED_PULLUP_P1
bool UserManager::GetSaveTrainingAssistedPullupP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingAssistedPullupP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Assisted_Pullup_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - BULGARIAN_SPLIT_SQUAT_P1
bool UserManager::GetSaveTrainingBulgarianSplitSquatP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingBulgarianSplitSquatP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Bulgarian_Split_Squat_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - SHOULDER_PRESS_P1
bool UserManager::GetSaveTrainingShoulderPressP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingShoulderPressP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Shoulder_Press_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - CABLE_FLY_P1
bool UserManager::GetSaveTrainingCableFlyP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingCableFlyP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Cable_Fly_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - LEG_EXTENSION_P1
bool UserManager::GetSaveTrainingLegExtensionP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingLegExtensionP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Leg_Extension_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - PLANK_P1
bool UserManager::GetSaveTrainingPlankP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingPlankP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Plank_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}

// ✅ GET TRAINING - CHEST_FLY_P1
bool UserManager::GetSaveTrainingChestFlyP1(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingChestFlyP1 ===" << "User:" << user_id;

    QSqlQuery query(db);
    query.prepare("SELECT Approach1, Approach2, Approach3 FROM Save_Training_Chest_Fly_P1 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);

    if (!query.exec()) return false;
    if (query.next()) {
        approach1 = query.value(0).toFloat();
        approach2 = query.value(1).toFloat();
        approach3 = query.value(2).toFloat();
        return true;
    }
    return false;
}


// ============================================================
// ✅ ПРОГРЕСС ПО НЕДЕЛЯМ
// ============================================================

// Инициализация/получение прогресса пользователя
bool UserManager::getWorkoutProgress(int user_id, int &current_week,
                                     bool &w1_done, bool &w2_done, bool &w3_done,
                                     QString &w1_date, QString &w2_date, QString &w3_date,
                                     bool &plan_completed)
{
    QSqlQuery q(db);
    q.prepare("SELECT current_week, workout1_done, workout2_done, workout3_done, "
              "       w1_date, w2_date, w3_date, week_completed "
              "FROM Training_Week_Progress WHERE user_id=:uid");
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "getWorkoutProgress SELECT error:" << q.lastError().text();
        return false;
    }

    if (!q.next()) {
        // Первый запуск — создаём запись
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO Training_Week_Progress "
                    "(user_id, current_week, workout1_done, workout2_done, workout3_done, "
                    " w1_date, w2_date, w3_date, week_start_date, week_completed) "
                    "VALUES (:uid, 1, 0, 0, 0, '', '', '', :date, 0)");
        ins.bindValue(":uid",  user_id);
        ins.bindValue(":date", QDate::currentDate().toString(Qt::ISODate));
        if (!ins.exec()) {
            qWarning() << "getWorkoutProgress INSERT error:" << ins.lastError().text();
            return false;
        }
        current_week = 1;
        w1_done = w2_done = w3_done = false;
        w1_date = w2_date = w3_date = "";
        plan_completed = false;
        return true;
    }

    current_week      = q.value(0).toInt();
    w1_done           = q.value(1).toBool();
    w2_done           = q.value(2).toBool();
    w3_done           = q.value(3).toBool();
    w1_date           = q.value(4).toString();
    w2_date           = q.value(5).toString();
    w3_date           = q.value(6).toString();
    bool weekCompleted = q.value(7).toBool();
    // План завершён: 12-я неделя И все тренировки сделаны И week_completed=1
    plan_completed = (current_week >= 12 && weekCompleted);
    return true;
}

// Отмечает тренировку как выполненную.
// Если все 3 пройдены — автоматически переходит на следующую неделю (если < 12).
bool UserManager::markWorkoutDone(int user_id, int workout_num, const QString &date,
                                  int &kachballs_awarded)
{
    qDebug() << "=== markWorkoutDone === user:" << user_id
             << "workout:" << workout_num << "date:" << date;

    kachballs_awarded = 0;
    if (workout_num < 1 || workout_num > 3) return false;

    // Получаем текущий прогресс (и создаём запись если нет)
    int cw; bool w1, w2, w3, pc; QString d1, d2, d3;
    if (!getWorkoutProgress(user_id, cw, w1, w2, w3, d1, d2, d3, pc)) return false;

    // Если эта тренировка уже отмечена — ничего не делаем (идемпотентность)
    if ((workout_num == 1 && w1) ||
        (workout_num == 2 && w2) ||
        (workout_num == 3 && w3)) {
        qDebug() << "markWorkoutDone: workout" << workout_num << "already done";
        return true;
    }

    // Отмечаем нужную тренировку
    QString col  = QString("workout%1_done").arg(workout_num);
    QString dcol = QString("w%1_date").arg(workout_num);

    QSqlQuery q(db);
    q.prepare(QString("UPDATE Training_Week_Progress SET %1=1, %2=:date WHERE user_id=:uid")
                  .arg(col).arg(dcol));
    q.bindValue(":date", date);
    q.bindValue(":uid",  user_id);

    if (!q.exec()) {
        qWarning() << "markWorkoutDone UPDATE error:" << q.lastError().text();
        return false;
    }

    // Проверяем все ли три теперь выполнены
    bool nw1 = (workout_num == 1) ? true : w1;
    bool nw2 = (workout_num == 2) ? true : w2;
    bool nw3 = (workout_num == 3) ? true : w3;

    if (nw1 && nw2 && nw3) {
        // Все три пройдены — ставим флаг week_completed=1
        // Реальный переход на следующую неделю произойдёт в tryAdvanceWeek
        // только в следующий понедельник
        QSqlQuery wc(db);
        wc.prepare("UPDATE Training_Week_Progress SET week_completed=1 WHERE user_id=:uid");
        wc.bindValue(":uid", user_id);
        if (!wc.exec())
            qWarning() << "week_completed flag error:" << wc.lastError().text();
        else
            qDebug() << "✅ Week" << cw << "all workouts done! Waiting for next Monday to advance.";

        if (cw >= 12) {
            qDebug() << "✅ PLAN COMPLETED for user" << user_id;
        }
    }

    // 💰 Призовые баллы за тренировку (сумма редактируется через админ-панель)
    int reward = getBonusAmount("workout_done", 15);
    if (reward > 0 && addKachballs(user_id, reward)) {
        kachballs_awarded = reward;
        qDebug() << "💰 Workout reward +" << reward << "kachballs for user" << user_id;
    }

    return true;
}

// Вызывается при каждом GET_WORKOUT_PROGRESS.
// Логика:
//   - Если week_completed=1 (все 3 тренировки сделаны) И наступил следующий понедельник
//     → переходим на week+1, сбрасываем флаги
//   - Если week_completed=0 (не все тренировки) И неделя уже кончилась (прошёл пн)
//     → сбрасываем флаги, неделю НЕ увеличиваем (штраф)

void UserManager::tryAdvanceWeek(int user_id)
{
    QSqlQuery q(db);
    q.prepare("SELECT current_week, workout1_done, workout2_done, workout3_done, "
              "week_start_date, week_completed FROM Training_Week_Progress WHERE user_id=:uid");
    q.bindValue(":uid", user_id);

    if (!q.exec() || !q.next()) return;

    int cw          = q.value(0).toInt();
    bool w1         = q.value(1).toBool();
    bool w2         = q.value(2).toBool();
    bool w3         = q.value(3).toBool();
    QString wsd     = q.value(4).toString();
    bool weekCompleted = q.value(5).toBool();

    if (wsd.isEmpty()) return;

#ifdef TEST_WEEK_ADVANCE
    // 🧪 ТЕСТ: "неделя" = 2 минуты. week_start_date хранится как datetime.
    QDateTime weekStart = QDateTime::fromString(wsd, Qt::ISODate);
    if (!weekStart.isValid())
        weekStart = QDateTime(QDate::fromString(wsd, Qt::ISODate), QTime(0, 0)); // совместимость
    QDateTime deadline = weekStart.addSecs(2 * 60); // 2 минуты
    bool weekExpired = (QDateTime::currentDateTime() >= deadline);
    qDebug() << "🧪 TEST_WEEK_ADVANCE: weekStart=" << weekStart.toString()
             << "deadline=" << deadline.toString()
             << "expired=" << weekExpired;
#else
    // PRODUCTION: следующий понедельник после старта недели
    QDate weekStart = QDate::fromString(wsd, Qt::ISODate);
    if (!weekStart.isValid()) return;
    int daysToMon = (8 - weekStart.dayOfWeek()) % 7;
    if (daysToMon == 0) daysToMon = 7;
    QDate nextMonday = weekStart.addDays(daysToMon);
    bool weekExpired = (QDate::currentDate() >= nextMonday);
#endif

    if (!weekExpired) {
        // Неделя ещё идёт — ничего не делаем
        return;
    }

    QDate today = QDate::currentDate();

    // Наступил понедельник (или 2 минуты в тесте)
    if (weekCompleted) {
        // Все 3 тренировки были пройдены — переходим на следующую неделю
        if (cw < 12) {
            QSqlQuery adv(db);
            adv.prepare("UPDATE Training_Week_Progress "
                        "SET current_week=:nw, workout1_done=0, workout2_done=0, workout3_done=0, "
                        "    w1_date='', w2_date='', w3_date='', "
                        "    week_start_date=:date, week_completed=0 "
                        "WHERE user_id=:uid");
            adv.bindValue(":nw",   cw + 1);
            adv.bindValue(":date", today.toString(Qt::ISODate));
            adv.bindValue(":uid",  user_id);
            if (!adv.exec())
                qWarning() << "tryAdvanceWeek advance error:" << adv.lastError().text();
            else
                qDebug() << "✅ tryAdvanceWeek: week expired, advancing" << cw << "→" << (cw + 1);
        } else {
            qDebug() << "✅ tryAdvanceWeek: Plan already completed for user" << user_id;
        }
    } else {
        // Неделя прошла, а не все тренировки сделаны — штраф
        qDebug() << "tryAdvanceWeek: week" << cw
                 << "expired without completing all workouts. Resetting flags (penalty).";
        QSqlQuery upd(db);
        upd.prepare("UPDATE Training_Week_Progress "
                    "SET workout1_done=0, workout2_done=0, workout3_done=0, "
                    "    w1_date='', w2_date='', w3_date='', "
                    "    week_start_date=:date, week_completed=0 "
                    "WHERE user_id=:uid");
        upd.bindValue(":date", today.toString(Qt::ISODate));
        upd.bindValue(":uid",  user_id);
        upd.exec();
    }
}

// ✅ Сброс прогресса тренировок — начать программу заново с недели 1
bool UserManager::resetWorkoutProgress(int user_id)
{
    qDebug() << "=== resetWorkoutProgress === user:" << user_id;
    QString today = QDate::currentDate().toString(Qt::ISODate);
    QSqlQuery q(db);
    q.prepare("UPDATE Training_Week_Progress "
              "SET current_week=1, "
              "    workout1_done=0, workout2_done=0, workout3_done=0, "
              "    w1_date='', w2_date='', w3_date='', "
              "    week_start_date=:date, "
              "    week_completed=0 "
              "WHERE user_id=:uid");
    q.bindValue(":date", today);
    q.bindValue(":uid",  user_id);
    if (!q.exec()) {
        qWarning() << "resetWorkoutProgress error:" << q.lastError().text();
        return false;
    }
    qDebug() << "✅ resetWorkoutProgress: progress cleared for user" << user_id;
    return true;
}

// ---------------------------------------------------------------------------
// Сохранить или обновить отзыв пользователя к рецепту
// ---------------------------------------------------------------------------
bool UserManager::saveReview(int recipe_id, int user_id, const QString &user_name,
                             int rating, const QString &comment)
{
    // INSERT OR REPLACE — обновляем если уже оставлял отзыв (UNIQUE recipe_id+user_id)
    QSqlQuery q(db);
    if (!q.prepare(
            "INSERT INTO Recipe_Reviews (recipe_id, user_id, user_name, rating, comment, created_at) "
            "VALUES (:rid, :uid, :name, :rating, :comment, datetime('now')) "
            "ON CONFLICT(recipe_id, user_id) DO UPDATE SET "
            "  rating=excluded.rating, "
            "  comment=excluded.comment, "
            "  user_name=excluded.user_name, "
            "  created_at=excluded.created_at")) {
        qWarning() << "saveReview prepare failed:" << q.lastError().text();
        return false;
    }
    q.bindValue(":rid",     recipe_id);
    q.bindValue(":uid",     user_id);
    q.bindValue(":name",    user_name);
    q.bindValue(":rating",  rating);
    q.bindValue(":comment", comment);
    if (!q.exec()) {
        qWarning() << "saveReview exec failed:" << q.lastError().text();
        return false;
    }

    // Пересчитываем средний рейтинг рецепта
    QSqlQuery avg(db);
    avg.prepare("UPDATE Recipes SET rating = "
                "(SELECT ROUND(AVG(rating), 1) FROM Recipe_Reviews WHERE recipe_id = :rid) "
                "WHERE id = :rid");
    avg.bindValue(":rid", recipe_id);
    avg.exec();

    qDebug() << "saveReview: recipe=" << recipe_id << "user=" << user_id << "rating=" << rating;
    return true;
}

// ---------------------------------------------------------------------------
// Загрузить все отзывы для рецепта → JSON-массив
// ---------------------------------------------------------------------------
bool UserManager::loadReviews(int recipe_id, QString &jsonData)
{
    QSqlQuery q(db);
    q.prepare("SELECT id, user_id, user_name, rating, comment, created_at "
              "FROM Recipe_Reviews "
              "WHERE recipe_id = :rid "
              "ORDER BY created_at DESC");
    q.bindValue(":rid", recipe_id);
    if (!q.exec()) {
        qWarning() << "loadReviews failed:" << q.lastError().text();
        return false;
    }

    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["reviewId"]  = q.value(0).toInt();
        obj["userId"]    = q.value(1).toInt();
        obj["userName"]  = q.value(2).toString();
        obj["rating"]    = q.value(3).toInt();
        obj["comment"]   = q.value(4).toString();
        obj["createdAt"] = q.value(5).toString();
        arr.append(obj);
    }

    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "loadReviews: recipe=" << recipe_id << "count=" << arr.size();
    return true;
}

bool UserManager::saveDailyNutrition(int user_id, const QString &date,
                                     const QString &mealsJson,
                                     int kcal, int protein, int fat, int carbs)
{
    // QSQLITE + QSqlQuery::prepare даёт «Parameter count mismatch» даже на простом DELETE
    // (вероятно несовместимость сборки). Обходим prepare: литералы с экранированием ' → ''.
    const QString eDate = escapeSqlLiteral(date);
    const QString eJson = escapeSqlLiteral(mealsJson);

    QSqlQuery q(db);
    const QString delSql = QStringLiteral(
                               "DELETE FROM Daily_Nutrition_Log WHERE user_id=%1 AND log_date='%2'")
                               .arg(user_id)
                               .arg(eDate);
    if (!q.exec(delSql)) {
        const QString errText = q.lastError().text();
        qWarning() << "saveDailyNutrition DELETE error:" << errText;

        // На старой версии схемы в FK было REFERENCES USERS(id), что не соответствует текущей таблице users(user_id).
        // Это ломает и DELETE, и INSERT (foreign key mismatch), поэтому попробуем пересоздать таблицу один раз.
        if (errText.contains("foreign key mismatch", Qt::CaseInsensitive))
        {
            qWarning() << "saveDailyNutrition: FK mismatch detected, rebuilding Daily_Nutrition_Log...";

            auto rebuildDailyNutritionLog = [this]() -> bool {
                if (!db.transaction()) return false;

                // На случай предыдущей неуспешной попытки миграции
                {
                    QSqlQuery cleanup(db);
                    cleanup.exec("DROP TABLE IF EXISTS Daily_Nutrition_Log_old");
                }

                QSqlQuery rn(db);
                if (!rn.exec("ALTER TABLE Daily_Nutrition_Log RENAME TO Daily_Nutrition_Log_old")) {
                    db.rollback();
                    qWarning() << "Daily_Nutrition_Log rebuild: rename failed:" << rn.lastError().text();
                    return false;
                }

                QSqlQuery createNew(db);
                const QString createNewSql =
                    "CREATE TABLE IF NOT EXISTS Daily_Nutrition_Log ("
                    "id         INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "user_id    INTEGER NOT NULL, "
                    "log_date   TEXT    NOT NULL, "
                    "meals_json TEXT    DEFAULT '[]', "
                    "total_kcal    INTEGER DEFAULT 0, "
                    "total_protein INTEGER DEFAULT 0, "
                    "total_fat     INTEGER DEFAULT 0, "
                    "total_carbs   INTEGER DEFAULT 0, "
                    "FOREIGN KEY(user_id) REFERENCES USERS(user_id), "
                    "UNIQUE(user_id, log_date))";

                if (!createNew.exec(createNewSql)) {
                    db.rollback();
                    qWarning() << "Daily_Nutrition_Log rebuild: create failed:" << createNew.lastError().text();
                    return false;
                }

                QSqlQuery copy(db);
                const QString copySql =
                    "INSERT INTO Daily_Nutrition_Log (id, user_id, log_date, meals_json, "
                    "total_kcal, total_protein, total_fat, total_carbs) "
                    "SELECT id, user_id, log_date, meals_json, "
                    "total_kcal, total_protein, total_fat, total_carbs "
                    "FROM Daily_Nutrition_Log_old";

                if (!copy.exec(copySql)) {
                    db.rollback();
                    qWarning() << "Daily_Nutrition_Log rebuild: copy failed:" << copy.lastError().text();
                    return false;
                }

                QSqlQuery drop(db);
                if (!drop.exec("DROP TABLE Daily_Nutrition_Log_old")) {
                    db.rollback();
                    qWarning() << "Daily_Nutrition_Log rebuild: drop old failed:" << drop.lastError().text();
                    return false;
                }

                return db.commit();
            };

            if (rebuildDailyNutritionLog()) {
                // Повторяем DELETE после успешной реконструкции схемы.
                if (!q.exec(delSql)) {
                    qWarning() << "saveDailyNutrition DELETE error after rebuild:" << q.lastError().text();
                    return false;
                }
            } else {
                qWarning() << "saveDailyNutrition: rebuild failed";
                return false;
            }
        } else {
            return false;
        }
    }

    const QString insSql = QStringLiteral(
                               "INSERT INTO Daily_Nutrition_Log (user_id, log_date, meals_json, "
                               "total_kcal, total_protein, total_fat, total_carbs) "
                               "VALUES (%1, '%2', '%3', %4, %5, %6, %7)")
                               .arg(user_id)
                               .arg(eDate)
                               .arg(eJson)
                               .arg(kcal)
                               .arg(protein)
                               .arg(fat)
                               .arg(carbs);

    if (!q.exec(insSql)) {
        qWarning() << "saveDailyNutrition INSERT error:" << q.lastError().text();
        return false;
    }
    qDebug() << "Daily nutrition saved for user" << user_id << "date" << date;
    return true;
}

bool UserManager::loadDailyNutrition(int user_id, const QString &date,
                                     QString &mealsJson,
                                     int &kcal, int &protein, int &fat, int &carbs)
{
    QSqlQuery q(db);
    q.prepare(
        "SELECT meals_json, total_kcal, total_protein, total_fat, total_carbs "
        "FROM Daily_Nutrition_Log WHERE user_id=:uid AND log_date=:date"
        );
    q.bindValue(":uid",  user_id);
    q.bindValue(":date", date);

    if (!q.exec()) {
        qWarning() << "loadDailyNutrition error:" << q.lastError().text();
        return false;
    }
    if (!q.next()) {
        mealsJson = "[]";
        kcal = protein = fat = carbs = 0;
        return true;   // нет записи — пустой день, это нормально
    }
    mealsJson = q.value(0).toString();
    kcal      = q.value(1).toInt();
    protein   = q.value(2).toInt();
    fat       = q.value(3).toInt();
    carbs     = q.value(4).toInt();
    return true;
}

bool UserManager::loadNutritionHistory(int user_id, QString &jsonData)
{
    QSqlQuery q(db);
    q.prepare(
        "SELECT log_date, meals_json, total_kcal, total_protein, total_fat, total_carbs "
        "FROM Daily_Nutrition_Log WHERE user_id=:uid "
        "ORDER BY log_date DESC LIMIT 90"
        );
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "loadNutritionHistory error:" << q.lastError().text();
        return false;
    }

    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["date"]     = q.value(0).toString();
        obj["meals"]    = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).array();
        obj["kcal"]     = q.value(2).toInt();
        obj["protein"]  = q.value(3).toInt();
        obj["fat"]      = q.value(4).toInt();
        obj["carbs"]    = q.value(5).toInt();
        arr.append(obj);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "Nutrition history loaded for user" << user_id << "days:" << arr.size();
    return true;
}

// ==================== ЗАМЕНЫ УПРАЖНЕНИЙ ====================

bool UserManager::saveExerciseSubstitution(int user_id, const QString &originalName, const QString &substituteName)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in saveExerciseSubstitution";
        return false;
    }

    QSqlQuery q(db);

    if (substituteName.isEmpty()) {
        // Delete substitution (restore original)
        q.prepare("DELETE FROM exercise_substitutions WHERE user_id = :uid AND original_name = :orig");
        q.bindValue(":uid",  user_id);
        q.bindValue(":orig", originalName);
    } else {
        // Upsert substitution
        q.prepare(R"(
            INSERT INTO exercise_substitutions (user_id, original_name, substitute_name)
            VALUES (:uid, :orig, :sub)
            ON CONFLICT(user_id, original_name)
            DO UPDATE SET substitute_name = excluded.substitute_name
        )");
        q.bindValue(":uid",  user_id);
        q.bindValue(":orig", originalName);
        q.bindValue(":sub",  substituteName);
    }

    if (!q.exec()) {
        qWarning() << "saveExerciseSubstitution error:" << q.lastError().text();
        return false;
    }

    qDebug() << "saveExerciseSubstitution OK: user" << user_id
             << "orig=" << originalName << "sub=" << substituteName;
    return true;
}

bool UserManager::loadExerciseSubstitutions(int user_id, QString &jsonData)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in loadExerciseSubstitutions";
        return false;
    }

    QSqlQuery q(db);
    q.prepare("SELECT original_name, substitute_name FROM exercise_substitutions WHERE user_id = :uid");
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "loadExerciseSubstitutions error:" << q.lastError().text();
        return false;
    }

    QJsonObject obj;
    while (q.next()) {
        obj[q.value(0).toString()] = q.value(1).toString();
    }

    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qDebug() << "loadExerciseSubstitutions OK: user" << user_id
             << "count=" << obj.size();
    return true;
}

// ==================== АДМИН-ПАНЕЛЬ ====================

bool UserManager::getAllUsers(QString &jsonData)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in getAllUsers";
        return false;
    }

    QSqlQuery q(db);
    if (!q.exec("SELECT user_id, first_name, last_name, email, gender, birth_date, height, weight, Goal, "
                "StandardSubscription, PlanTrainning, COALESCE(kachballs,0), "
                "COALESCE(VipSubscription, 0), COALESCE(subscription_frozen, 0), "
                "COALESCE(FeedbackSubscription, 0), COALESCE(freeze_ends_at, ''), "
                "COALESCE(freeze_count_year, 0) "
                "FROM USERS ORDER BY user_id")) {
        qWarning() << "getAllUsers error:" << q.lastError().text();
        return false;
    }

    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["user_id"]       = q.value(0).toInt();
        obj["first_name"]    = q.value(1).toString();
        obj["last_name"]     = q.value(2).toString();
        obj["email"]         = q.value(3).toString();
        obj["gender"]        = q.value(4).toString();
        obj["birth_date"]    = q.value(5).toString();
        obj["height"]        = q.value(6).toDouble();
        obj["weight"]        = q.value(7).toDouble();
        obj["goal"]          = q.value(8).toString();
        obj["subscription"]  = q.value(9).toInt();
        int planId = q.value(10).toInt();
        obj["planTrainning"] = planId;
        QString pn = "Нет плана";
        if (planId == 1) pn = "Зал с нуля";
        else if (planId == 2) pn = "Зал любитель";
        else if (planId == 3) pn = "Песочные часы";
        else if (planId == 4) pn = "Ягодицы без ног";
        else if (planId == 5) pn = "Песочные часы PRO";
        else if (planId == 7) pn = "Учимся подтягиваться";
        obj["planName"]      = pn;
        obj["kachballs"]     = q.value(11).toInt();
        obj["vipSubscription"]      = q.value(12).toInt();
        obj["frozen"]               = q.value(13).toInt();
        obj["feedbackSubscription"] = q.value(14).toInt();
        obj["freezeEndsAt"]         = q.value(15).toString();
        obj["freezeCountYear"]      = q.value(16).toInt();
        arr.append(obj);
    }

    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "getAllUsers OK: count=" << arr.size();
    return true;
}

bool UserManager::setSubscription(int user_id, int status)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in setSubscription";
        return false;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE USERS SET StandardSubscription = :status WHERE user_id = :uid");
    q.bindValue(":status", status);
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "setSubscription error:" << q.lastError().text();
        return false;
    }

    qDebug() << "setSubscription OK: user" << user_id << "status=" << status;
    return true;
}

// ✅ Заморозить/разморозить подписку (1 = заморожена, 0 = активна).
// Прогресс пользователя (тренировки, рацион, кач-баллы, VIP-программа) НЕ удаляется.
bool UserManager::setSubscriptionFrozen(int user_id, int frozen)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in setSubscriptionFrozen";
        return false;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE USERS SET subscription_frozen = :f WHERE user_id = :uid");
    q.bindValue(":f", frozen ? 1 : 0);
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "setSubscriptionFrozen error:" << q.lastError().text();
        return false;
    }
    qDebug() << "setSubscriptionFrozen OK: user" << user_id << "frozen=" << frozen;
    return true;
}

bool UserManager::getSubscriptionFrozen(int user_id, int &frozen)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT COALESCE(subscription_frozen, 0) FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;
    frozen = q.value(0).toInt();
    return true;
}

// ─── Заморозка с лимитом 2 раза в год и авто-разморозкой через days дней ───
bool UserManager::freezeSubscription(int user_id, int days, QString &errorMessage)
{
    if (!db.isOpen()) { errorMessage = "DB not open"; return false; }

    // Ограничения: 7–14 дней по ТЗ
    if (days < 7) days = 7;
    if (days > 14) days = 14;

    const int currentYear = QDate::currentDate().year();

    QSqlQuery sel(db);
    sel.prepare("SELECT COALESCE(freeze_count_year,0), COALESCE(freeze_year,0), "
                "COALESCE(subscription_frozen,0) FROM USERS WHERE user_id=:uid");
    sel.bindValue(":uid", user_id);
    if (!sel.exec() || !sel.next()) { errorMessage = "Пользователь не найден"; return false; }

    int count = sel.value(0).toInt();
    int year  = sel.value(1).toInt();
    int already = sel.value(2).toInt();

    if (already) { errorMessage = "Подписка уже заморожена"; return false; }

    // Сброс счётчика при смене года
    if (year != currentYear) { count = 0; year = currentYear; }

    if (count >= 2) {
        errorMessage = "Лимит заморозок исчерпан (2 раза в год)";
        return false;
    }

    QString endsAt = QDateTime::currentDateTimeUtc().addDays(days).toString(Qt::ISODate);

    QSqlQuery upd(db);
    upd.prepare("UPDATE USERS SET subscription_frozen=1, freeze_ends_at=:ends, "
                "freeze_count_year=:cnt, freeze_year=:yr WHERE user_id=:uid");
    upd.bindValue(":ends", endsAt);
    upd.bindValue(":cnt", count + 1);
    upd.bindValue(":yr", year);
    upd.bindValue(":uid", user_id);
    if (!upd.exec()) { errorMessage = upd.lastError().text(); return false; }

    qDebug() << "❄ Frozen user" << user_id << "until" << endsAt << "(count" << (count+1) << "/2)";
    return true;
}

bool UserManager::unfreezeSubscription(int user_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE USERS SET subscription_frozen=0, freeze_ends_at='' WHERE user_id=:uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    qDebug() << "☀ Unfrozen user" << user_id;
    return true;
}

bool UserManager::getExpiredFreezes(QList<int> &userIds)
{
    userIds.clear();
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    q.prepare("SELECT user_id FROM USERS WHERE subscription_frozen=1 "
              "AND freeze_ends_at != '' AND freeze_ends_at <= :now");
    q.bindValue(":now", now);
    if (!q.exec()) return false;
    while (q.next()) userIds.append(q.value(0).toInt());
    return true;
}

// ─── 3-й тариф: подписка с обратной связью ───
bool UserManager::setFeedbackSubscription(int user_id, int status)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE USERS SET FeedbackSubscription = :s WHERE user_id = :uid");
    q.bindValue(":s", status ? 1 : 0);
    q.bindValue(":uid", user_id);
    if (!q.exec()) {
        qWarning() << "setFeedbackSubscription error:" << q.lastError().text();
        return false;
    }
    qDebug() << "Feedback subscription set:" << user_id << "=" << status;
    return true;
}

bool UserManager::getFeedbackSubscription(int user_id, int &status)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT COALESCE(FeedbackSubscription, 0) FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;
    status = q.value(0).toInt();
    return true;
}

// ─── Авто-биллинг (рекуррентные платежи YooKassa) ────────────────────────
// Здесь храним только метаданные (payment_method_id, дата следующего списания, retry).
// Сам платёж делает сайт (Python/Flask), общаясь с YooKassa.
// При неуспехе после 3 попыток ставим subscription_frozen=1 и freeze_reason='billing'.
// Эта заморозка НЕ расходует пользовательскую квоту 2/год (она ведётся через freeze_count_year).

bool UserManager::billingSaveMethod(int user_id,
                                    const QString &paymentMethodId,
                                    const QString &paymentMethodTitle,
                                    const QString &tier,
                                    int periodDays)
{
    if (!db.isOpen()) return false;
    if (tier != "lite" && tier != "feedback") {
        qWarning() << "billingSaveMethod: unsupported tier" << tier;
        return false;
    }
    // Защита от попадания «мусорных» строк в БД: пустой payment_method_id
    // ломает фильтр в billingGetDue() и приводит к попыткам списания с
    // несуществующей карты в YooKassa.
    if (paymentMethodId.trimmed().isEmpty()) {
        qWarning() << "billingSaveMethod: paymentMethodId is empty for user" << user_id;
        return false;
    }

    const QString nextDate =
        QDateTime::currentDateTimeUtc().addDays(periodDays).toString(Qt::ISODate);

    QSqlQuery q(db);
    q.prepare(
        "UPDATE USERS SET "
        "  payment_method_id       = :pmid, "
        "  payment_method_title    = :pmtitle, "
        "  subscription_tier       = :tier, "
        "  subscription_auto_renew = 1, "
        "  next_billing_date       = :next, "
        "  billing_retry_count     = 0, "
        "  last_billing_attempt_at = :now, "
        "  last_billing_status     = 'ok' "
        "WHERE user_id = :uid");
    q.bindValue(":pmid",    paymentMethodId);
    q.bindValue(":pmtitle", paymentMethodTitle);
    q.bindValue(":tier",    tier);
    q.bindValue(":next",    nextDate);
    q.bindValue(":now",     QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    q.bindValue(":uid",     user_id);
    if (!q.exec()) {
        qWarning() << "billingSaveMethod error:" << q.lastError().text();
        return false;
    }
    qDebug() << "💳 Billing saved: user" << user_id
             << "tier" << tier << "next" << nextDate;
    return true;
}

bool UserManager::billingGetDue(QList<BillingDueRow> &rows)
{
    rows.clear();
    if (!db.isOpen()) return false;

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QSqlQuery q(db);
    q.prepare(
        "SELECT user_id, subscription_tier, payment_method_id, "
        "       COALESCE(billing_retry_count, 0), "
        "       COALESCE(email, ''), COALESCE(first_name, '') "
        "FROM USERS "
        "WHERE subscription_auto_renew = 1 "
        "  AND payment_method_id != '' "
        "  AND subscription_tier IN ('lite', 'feedback') "
        "  AND COALESCE(VipSubscription, 0) = 0 "
        "  AND next_billing_date != '' "
        "  AND next_billing_date <= :now");
    q.bindValue(":now", now);
    if (!q.exec()) {
        qWarning() << "billingGetDue error:" << q.lastError().text();
        return false;
    }
    while (q.next()) {
        BillingDueRow r;
        r.userId          = q.value(0).toInt();
        r.tier            = q.value(1).toString();
        r.paymentMethodId = q.value(2).toString();
        r.retryCount      = q.value(3).toInt();
        r.email           = q.value(4).toString();
        r.firstName       = q.value(5).toString();
        rows.append(r);
    }
    return true;
}

bool UserManager::billingRecordSuccess(int user_id, int periodDays)
{
    if (!db.isOpen()) return false;

    const QString next = QDateTime::currentDateTimeUtc().addDays(periodDays).toString(Qt::ISODate);
    const QString now  = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QSqlQuery q(db);
    // Если до этого была биллинг-заморозка — снимаем её.
    // Ручную заморозку (freeze_reason='manual') НЕ трогаем.
    q.prepare(
        "UPDATE USERS SET "
        "  next_billing_date        = :next, "
        "  billing_retry_count      = 0, "
        "  last_billing_attempt_at  = :now, "
        "  last_billing_status      = 'ok', "
        "  subscription_frozen      = CASE WHEN freeze_reason='billing' THEN 0 "
        "                                  ELSE subscription_frozen END, "
        "  freeze_ends_at           = CASE WHEN freeze_reason='billing' THEN '' "
        "                                  ELSE freeze_ends_at END, "
        "  freeze_reason            = CASE WHEN freeze_reason='billing' THEN '' "
        "                                  ELSE freeze_reason END "
        "WHERE user_id = :uid");
    q.bindValue(":next", next);
    q.bindValue(":now",  now);
    q.bindValue(":uid",  user_id);
    if (!q.exec()) {
        qWarning() << "billingRecordSuccess error:" << q.lastError().text();
        return false;
    }
    qDebug() << "✅ Billing success: user" << user_id << "next" << next;
    return true;
}

bool UserManager::billingRecordFailure(int user_id, bool &outFrozen)
{
    outFrozen = false;
    if (!db.isOpen()) return false;

    // Получаем текущий retry_count
    int retry = 0;
    {
        QSqlQuery sel(db);
        sel.prepare("SELECT COALESCE(billing_retry_count, 0) FROM USERS WHERE user_id=:uid");
        sel.bindValue(":uid", user_id);
        if (!sel.exec() || !sel.next()) {
            qWarning() << "billingRecordFailure: user not found" << user_id;
            return false;
        }
        retry = sel.value(0).toInt();
    }

    // Если пользователь уже заморожен по биллингу после 3 неудач, повторные
    // попытки больше не наращивают счётчик — оставляем 3, чтобы поле в БД не
    // росло бесконечно и не путало диагностику.
    if (retry >= 3) {
        qDebug() << "billingRecordFailure: user" << user_id
                 << "already at retry cap (3), no further increment";
        outFrozen = true;
        return true;
    }

    const int newRetry = retry + 1;
    const QString now  = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (newRetry >= 3) {
        // Третья подряд неудача — морозим подписку (без квоты 2/год).
        // Подписка-флаги (StandardSubscription/FeedbackSubscription) НЕ снимаем —
        // данные пользователя сохраняются, доступ блокируется через subscription_frozen.
        QSqlQuery upd(db);
        upd.prepare(
            "UPDATE USERS SET "
            "  billing_retry_count     = :r, "
            "  last_billing_attempt_at = :now, "
            "  last_billing_status     = 'failed', "
            "  subscription_frozen     = 1, "
            "  freeze_reason           = 'billing', "
            "  freeze_ends_at          = '' "
            "WHERE user_id = :uid");
        upd.bindValue(":r",   newRetry);
        upd.bindValue(":now", now);
        upd.bindValue(":uid", user_id);
        if (!upd.exec()) {
            qWarning() << "billingRecordFailure (freeze) error:" << upd.lastError().text();
            return false;
        }
        outFrozen = true;
        qDebug() << "❄ Billing FROZEN: user" << user_id << "after 3 failed attempts";
        return true;
    }

    // Иначе — переносим next_billing_date: +1 / +3 / +5 дней.
    int delayDays = 1;
    if (newRetry == 1)      delayDays = 1;
    else if (newRetry == 2) delayDays = 3;

    const QString next = QDateTime::currentDateTimeUtc().addDays(delayDays).toString(Qt::ISODate);

    QSqlQuery upd(db);
    upd.prepare(
        "UPDATE USERS SET "
        "  billing_retry_count     = :r, "
        "  next_billing_date       = :next, "
        "  last_billing_attempt_at = :now, "
        "  last_billing_status     = 'retry' "
        "WHERE user_id = :uid");
    upd.bindValue(":r",    newRetry);
    upd.bindValue(":next", next);
    upd.bindValue(":now",  now);
    upd.bindValue(":uid",  user_id);
    if (!upd.exec()) {
        qWarning() << "billingRecordFailure (retry) error:" << upd.lastError().text();
        return false;
    }
    qDebug() << "⚠ Billing retry" << newRetry << "for user" << user_id
             << "next attempt" << next;
    return true;
}

bool UserManager::billingCancelAutoRenew(int user_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(
        "UPDATE USERS SET "
        "  subscription_auto_renew = 0, "
        "  next_billing_date       = '', "
        "  billing_retry_count     = 0 "
        "WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) {
        qWarning() << "billingCancelAutoRenew error:" << q.lastError().text();
        return false;
    }
    qDebug() << "🛑 Auto-renew cancelled for user" << user_id;
    return true;
}

bool UserManager::billingGetState(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(
        "SELECT COALESCE(subscription_tier, ''), "
        "       COALESCE(payment_method_title, ''), "
        "       COALESCE(next_billing_date, ''), "
        "       COALESCE(subscription_auto_renew, 0), "
        "       COALESCE(subscription_frozen, 0), "
        "       COALESCE(freeze_reason, ''), "
        "       COALESCE(last_billing_status, ''), "
        "       COALESCE(billing_retry_count, 0), "
        "       COALESCE(StandardSubscription, 0), "
        "       COALESCE(FeedbackSubscription, 0), "
        "       COALESCE(VipSubscription, 0) "
        "FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;

    QJsonObject obj;
    obj["tier"]                    = q.value(0).toString();
    obj["payment_method_title"]    = q.value(1).toString();
    obj["next_billing_date"]       = q.value(2).toString();
    obj["auto_renew"]              = q.value(3).toInt() == 1;
    obj["frozen"]                  = q.value(4).toInt() == 1;
    obj["freeze_reason"]           = q.value(5).toString();
    obj["last_billing_status"]     = q.value(6).toString();
    obj["billing_retry_count"]     = q.value(7).toInt();
    obj["standard_subscription"]   = q.value(8).toInt() == 1;
    obj["feedback_subscription"]   = q.value(9).toInt() == 1;
    obj["vip_subscription"]        = q.value(10).toInt() == 1;

    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}

// ─── Чат ───
bool UserManager::sendChatMessage(int user_id, bool fromAdmin, const QString &text, qint64 &messageId)
{
    if (!db.isOpen() || text.trimmed().isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT INTO Chat_Messages (user_id, from_admin, text, read_by_user, read_by_admin) "
              "VALUES (:uid, :fa, :txt, :ru, :ra)");
    q.bindValue(":uid", user_id);
    q.bindValue(":fa", fromAdmin ? 1 : 0);
    q.bindValue(":txt", text.left(4000));   // лимит длины сообщения
    q.bindValue(":ru", fromAdmin ? 0 : 1);  // отправитель уже "прочитал"
    q.bindValue(":ra", fromAdmin ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "sendChatMessage error:" << q.lastError().text();
        return false;
    }
    messageId = q.lastInsertId().toLongLong();
    return true;
}

bool UserManager::loadChatHistory(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    // ✅ Возвращаем createdAt как ISO 8601 с суффиксом 'Z' — чтобы JS на клиенте
    //    однозначно парсил как UTC и корректно конвертировал в локальное время.
    // hasPhoto = LENGTH(photo) > 0; сам бинарь не отдаём в истории, его
    // подгружают отдельной командой GET_CHAT_PHOTO|message_id.
    q.prepare("SELECT id, from_admin, text, "
              "strftime('%Y-%m-%dT%H:%M:%SZ', created_at) AS created_at_iso, "
              "read_by_user, read_by_admin, "
              "CASE WHEN photo IS NOT NULL AND length(photo) > 0 THEN 1 ELSE 0 END AS has_photo "
              "FROM Chat_Messages WHERE user_id=:uid ORDER BY id ASC LIMIT 500");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["id"]         = q.value(0).toInt();
        o["fromAdmin"]  = q.value(1).toInt();
        o["text"]       = q.value(2).toString();
        o["createdAt"]  = q.value(3).toString();
        o["readByUser"] = q.value(4).toInt();
        o["readByAdmin"]= q.value(5).toInt();
        o["hasPhoto"]   = q.value(6).toInt() == 1;
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

// ─── Чат: фото-сообщения ──────────────────────────────────────────────────
bool UserManager::sendChatPhoto(int user_id, bool fromAdmin,
                                const QByteArray &photoData, qint64 &messageId)
{
    if (!db.isOpen() || photoData.isEmpty()) return false;
    QSqlQuery q(db);
    // text оставляем пустой — клиент рисует bubble как «фото».
    // read-флаги ставятся как у обычного сообщения (отправитель уже видел).
    q.prepare("INSERT INTO Chat_Messages (user_id, from_admin, text, photo, read_by_user, read_by_admin) "
              "VALUES (:uid, :fa, '', :ph, :ru, :ra)");
    q.bindValue(":uid", user_id);
    q.bindValue(":fa", fromAdmin ? 1 : 0);
    q.bindValue(":ph", photoData);
    q.bindValue(":ru", fromAdmin ? 0 : 1);
    q.bindValue(":ra", fromAdmin ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "sendChatPhoto error:" << q.lastError().text();
        return false;
    }
    messageId = q.lastInsertId().toLongLong();
    qDebug() << "📷 Chat photo saved: user" << user_id
             << "fromAdmin=" << fromAdmin
             << "size=" << photoData.size() << "msgId=" << messageId;
    return true;
}

bool UserManager::getChatPhoto(qint64 message_id, QByteArray &photoData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT photo FROM Chat_Messages WHERE id=:mid");
    q.bindValue(":mid", message_id);
    if (!q.exec() || !q.next()) {
        qWarning() << "getChatPhoto: message not found id=" << message_id;
        return false;
    }
    photoData = q.value(0).toByteArray();
    return !photoData.isEmpty();
}

bool UserManager::clearChatHistory(int user_id, int &deletedCount)
{
    deletedCount = 0;
    if (!db.isOpen() || user_id <= 0) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Chat_Messages WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) {
        qWarning() << "clearChatHistory error:" << q.lastError().text();
        return false;
    }
    deletedCount = q.numRowsAffected();
    qDebug() << "🗑 Chat history cleared for user" << user_id
             << "rows deleted:" << deletedCount;
    return true;
}

// ═════════════════════════════════════════════════════════════════════
// ✅ Чат техподдержки — отдельная таблица Support_Messages.
// Доступен ВСЕМ пользователям, не зависит от тарифа.
// ═════════════════════════════════════════════════════════════════════

bool UserManager::sendSupportMessage(int user_id, bool fromAdmin,
                                     const QString &text, qint64 &messageId)
{
    if (!db.isOpen() || text.trimmed().isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT INTO Support_Messages (user_id, from_admin, text, read_by_user, read_by_admin) "
              "VALUES (:uid, :fa, :txt, :ru, :ra)");
    q.bindValue(":uid", user_id);
    q.bindValue(":fa", fromAdmin ? 1 : 0);
    q.bindValue(":txt", text.left(4000));
    q.bindValue(":ru", fromAdmin ? 0 : 1);
    q.bindValue(":ra", fromAdmin ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "sendSupportMessage error:" << q.lastError().text();
        return false;
    }
    messageId = q.lastInsertId().toLongLong();
    return true;
}

bool UserManager::sendSupportPhoto(int user_id, bool fromAdmin,
                                   const QByteArray &photoData, qint64 &messageId)
{
    if (!db.isOpen() || photoData.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT INTO Support_Messages (user_id, from_admin, text, photo, read_by_user, read_by_admin) "
              "VALUES (:uid, :fa, '', :ph, :ru, :ra)");
    q.bindValue(":uid", user_id);
    q.bindValue(":fa", fromAdmin ? 1 : 0);
    q.bindValue(":ph", photoData);
    q.bindValue(":ru", fromAdmin ? 0 : 1);
    q.bindValue(":ra", fromAdmin ? 1 : 0);
    if (!q.exec()) {
        qWarning() << "sendSupportPhoto error:" << q.lastError().text();
        return false;
    }
    messageId = q.lastInsertId().toLongLong();
    return true;
}

bool UserManager::getSupportPhoto(qint64 message_id, QByteArray &photoData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT photo FROM Support_Messages WHERE id = :mid");
    q.bindValue(":mid", message_id);
    if (!q.exec() || !q.next()) return false;
    photoData = q.value(0).toByteArray();
    return !photoData.isEmpty();
}

bool UserManager::loadSupportHistory(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT id, from_admin, text, "
              "strftime('%Y-%m-%dT%H:%M:%SZ', created_at) AS created_at_iso, "
              "read_by_user, read_by_admin, "
              "CASE WHEN photo IS NOT NULL AND length(photo) > 0 THEN 1 ELSE 0 END AS has_photo "
              "FROM Support_Messages WHERE user_id = :uid ORDER BY id ASC LIMIT 500");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["id"]         = q.value(0).toInt();
        o["fromAdmin"]  = q.value(1).toInt();
        o["text"]       = q.value(2).toString();
        o["createdAt"]  = q.value(3).toString();
        o["readByUser"] = q.value(4).toInt();
        o["readByAdmin"]= q.value(5).toInt();
        o["hasPhoto"]   = q.value(6).toInt() == 1;
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::markSupportRead(int user_id, bool byAdmin)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (byAdmin) {
        q.prepare("UPDATE Support_Messages SET read_by_admin = 1 "
                  "WHERE user_id = :uid AND from_admin = 0");
    } else {
        q.prepare("UPDATE Support_Messages SET read_by_user = 1 "
                  "WHERE user_id = :uid AND from_admin = 1");
    }
    q.bindValue(":uid", user_id);
    return q.exec();
}

bool UserManager::countSupportUnreadForUser(int user_id, int &count)
{
    count = 0;
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT COUNT(*) FROM Support_Messages "
              "WHERE user_id = :uid AND from_admin = 1 AND read_by_user = 0");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;
    count = q.value(0).toInt();
    return true;
}

bool UserManager::getAdminSupportConversations(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(
        "SELECT u.user_id, u.first_name, u.last_name, "
        "  (SELECT text FROM Support_Messages sm WHERE sm.user_id = u.user_id ORDER BY sm.id DESC LIMIT 1) AS last_text, "
        "  (SELECT strftime('%Y-%m-%dT%H:%M:%SZ', created_at) FROM Support_Messages sm WHERE sm.user_id = u.user_id ORDER BY sm.id DESC LIMIT 1) AS last_at, "
        "  (SELECT COUNT(*) FROM Support_Messages sm WHERE sm.user_id = u.user_id AND sm.from_admin = 0 AND sm.read_by_admin = 0) AS unread "
        "FROM USERS u "
        "WHERE EXISTS (SELECT 1 FROM Support_Messages sm WHERE sm.user_id = u.user_id) "
        "ORDER BY last_at DESC");
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["user_id"]    = q.value(0).toInt();
        o["first_name"] = q.value(1).toString();
        o["last_name"]  = q.value(2).toString();
        o["last_text"]  = q.value(3).toString();
        o["last_at"]    = q.value(4).toString();
        o["unread"]     = q.value(5).toInt();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::clearSupportHistory(int user_id, int &deletedCount)
{
    deletedCount = 0;
    if (!db.isOpen() || user_id <= 0) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Support_Messages WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    deletedCount = q.numRowsAffected();
    return true;
}

bool UserManager::markChatRead(int user_id, bool byAdmin)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (byAdmin) {
        q.prepare("UPDATE Chat_Messages SET read_by_admin=1 WHERE user_id=:uid AND from_admin=0");
    } else {
        q.prepare("UPDATE Chat_Messages SET read_by_user=1 WHERE user_id=:uid AND from_admin=1");
    }
    q.bindValue(":uid", user_id);
    return q.exec();
}

bool UserManager::getAdminConversations(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec(
            "SELECT u.user_id, u.first_name, u.last_name, "
            "  (SELECT text FROM Chat_Messages cm WHERE cm.user_id=u.user_id ORDER BY cm.id DESC LIMIT 1) AS last_text, "
            "  (SELECT strftime('%Y-%m-%dT%H:%M:%SZ', created_at) FROM Chat_Messages cm WHERE cm.user_id=u.user_id ORDER BY cm.id DESC LIMIT 1) AS last_at, "
            "  (SELECT COUNT(*) FROM Chat_Messages cm WHERE cm.user_id=u.user_id AND cm.from_admin=0 AND cm.read_by_admin=0) AS unread "
            "FROM USERS u "
            "WHERE EXISTS (SELECT 1 FROM Chat_Messages cm WHERE cm.user_id=u.user_id) "
            "ORDER BY last_at DESC")) {
        qWarning() << "getAdminConversations error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["user_id"]    = q.value(0).toInt();
        o["first_name"] = q.value(1).toString();
        o["last_name"]  = q.value(2).toString();
        o["last_text"]  = q.value(3).toString();
        o["last_at"]    = q.value(4).toString();
        o["unread"]     = q.value(5).toInt();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::countUnreadForUser(int user_id, int &count)
{
    count = 0;
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT COUNT(*) FROM Chat_Messages WHERE user_id=:uid AND from_admin=1 AND read_by_user=0");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;
    count = q.value(0).toInt();
    return true;
}

// ─── Приготовленные блюда (фото → баллы) ───
bool UserManager::saveCookedDish(int user_id, int recipe_id, const QByteArray &photoData, int &dishId)
{
    if (!db.isOpen() || photoData.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT INTO Cooked_Dishes (user_id, recipe_id, photo) VALUES (:uid, :rid, :p)");
    q.bindValue(":uid", user_id);
    q.bindValue(":rid", recipe_id > 0 ? recipe_id : QVariant(QMetaType(QMetaType::Int)));
    q.bindValue(":p", photoData);
    if (!q.exec()) {
        qWarning() << "saveCookedDish error:" << q.lastError().text();
        return false;
    }
    dishId = q.lastInsertId().toInt();
    // Сумма редактируется через админ-панель (Bonus_Settings.cooked_dish)
    addKachballs(user_id, getBonusAmount("cooked_dish", 5));
    return true;
}

bool UserManager::loadCookedDishes(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT id, recipe_id, created_at, approved FROM Cooked_Dishes WHERE user_id=:uid "
              "ORDER BY id DESC LIMIT 100");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["id"]        = q.value(0).toInt();
        o["recipe_id"] = q.value(1).toInt();
        o["createdAt"] = q.value(2).toString();
        o["approved"]  = q.value(3).toInt();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::getCookedDishPhoto(int dish_id, QByteArray &photoData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT photo FROM Cooked_Dishes WHERE id=:id");
    q.bindValue(":id", dish_id);
    if (!q.exec() || !q.next()) return false;
    photoData = q.value(0).toByteArray();
    return !photoData.isEmpty();
}

// ─── Просмотренные уроки ───
bool UserManager::markLessonViewed(int user_id, const QString &lessonKey, int &awardedBalls)
{
    awardedBalls = 0;
    if (!db.isOpen() || lessonKey.trimmed().isEmpty()) return false;

    QSqlQuery chk(db);
    chk.prepare("SELECT 1 FROM User_Lessons_Viewed WHERE user_id=:uid AND lesson_key=:k");
    chk.bindValue(":uid", user_id);
    chk.bindValue(":k", lessonKey);
    if (chk.exec() && chk.next()) {
        return true;   // уже просмотрен — баллы не повторяются
    }

    QSqlQuery ins(db);
    ins.prepare("INSERT INTO User_Lessons_Viewed (user_id, lesson_key) VALUES (:uid, :k)");
    ins.bindValue(":uid", user_id);
    ins.bindValue(":k", lessonKey);
    if (!ins.exec()) {
        qWarning() << "markLessonViewed error:" << ins.lastError().text();
        return false;
    }
    // Сумма редактируется через админ-панель (Bonus_Settings.lesson_viewed)
    awardedBalls = getBonusAmount("lesson_viewed", 3);
    addKachballs(user_id, awardedBalls);
    return true;
}

bool UserManager::loadViewedLessons(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT lesson_key, viewed_at FROM User_Lessons_Viewed WHERE user_id=:uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["lesson_key"] = q.value(0).toString();
        o["viewed_at"]  = q.value(1).toString();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

// ─── Избранные рецепты ───
bool UserManager::addFavoriteRecipe(int user_id, int recipe_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT OR IGNORE INTO User_Favorite_Recipes (user_id, recipe_id) VALUES (:uid, :rid)");
    q.bindValue(":uid", user_id);
    q.bindValue(":rid", recipe_id);
    return q.exec();
}

bool UserManager::removeFavoriteRecipe(int user_id, int recipe_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM User_Favorite_Recipes WHERE user_id=:uid AND recipe_id=:rid");
    q.bindValue(":uid", user_id);
    q.bindValue(":rid", recipe_id);
    return q.exec();
}

bool UserManager::loadFavoriteRecipes(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT recipe_id FROM User_Favorite_Recipes WHERE user_id=:uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) arr.append(q.value(0).toInt());
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

// ─── Регистрация push-устройств ───
bool UserManager::registerDevice(int user_id, const QString &token, const QString &platform)
{
    if (!db.isOpen() || token.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT OR REPLACE INTO User_Devices (user_id, device_token, platform, last_seen) "
              "VALUES (:uid, :t, :p, datetime('now'))");
    q.bindValue(":uid", user_id);
    q.bindValue(":t", token);
    q.bindValue(":p", platform);
    return q.exec();
}

bool UserManager::unregisterDevice(int user_id, const QString &token)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM User_Devices WHERE user_id=:uid AND device_token=:t");
    q.bindValue(":uid", user_id);
    q.bindValue(":t", token);
    return q.exec();
}

bool UserManager::loadUserDevices(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT device_token, platform, last_seen FROM User_Devices WHERE user_id=:uid");
    q.bindValue(":uid", user_id);
    if (!q.exec()) return false;
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["token"]     = q.value(0).toString();
        o["platform"]  = q.value(1).toString();
        o["last_seen"] = q.value(2).toString();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::loadAllDevices(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    // INNER JOIN — отсекаем «осиротевшие» записи устройств для удалённых
    // пользователей (LEFT JOIN их пропускал, и сайт-cron пытался для них
    // делать платежи / слать пуши на несуществующих юзеров).
    q.prepare(
        "SELECT d.user_id, COALESCE(u.first_name, ''), d.device_token, d.platform "
        "FROM User_Devices d "
        "INNER JOIN USERS u ON u.user_id = d.user_id");
    if (!q.exec()) {
        qWarning() << "loadAllDevices error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["user_id"]    = q.value(0).toInt();
        o["first_name"] = q.value(1).toString();
        o["token"]      = q.value(2).toString();
        o["platform"]   = q.value(3).toString();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::removeDeviceByToken(const QString &token)
{
    if (!db.isOpen() || token.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM User_Devices WHERE device_token = :t");
    q.bindValue(":t", token);
    if (!q.exec()) {
        qWarning() << "removeDeviceByToken error:" << q.lastError().text();
        return false;
    }
    qDebug() << "🗑 Device unregistered (invalid token):" << token.left(12) << "…";
    return true;
}

bool UserManager::addKachballs(int user_id, int amount)
{
    if (!db.isOpen() || amount == 0) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE USERS SET kachballs = COALESCE(kachballs,0) + :a WHERE user_id=:uid");
    q.bindValue(":a", amount);
    q.bindValue(":uid", user_id);
    return q.exec();
}

// ── Настройки призовых баллов ──────────────────────────────────────────────
int UserManager::getBonusAmount(const QString &key, int defaultAmount)
{
    if (!db.isOpen()) return defaultAmount;
    QSqlQuery q(db);
    q.prepare("SELECT amount FROM Bonus_Settings WHERE key=:k");
    q.bindValue(":k", key);
    if (!q.exec() || !q.next()) return defaultAmount;
    return q.value(0).toInt();
}

bool UserManager::setBonusAmount(const QString &key, int amount)
{
    if (!db.isOpen() || key.isEmpty() || amount < 0) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE Bonus_Settings SET amount=:a WHERE key=:k");
    q.bindValue(":a", amount);
    q.bindValue(":k", key);
    if (!q.exec()) {
        qWarning() << "setBonusAmount error:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0) {
        // ключа нет — вставляем
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO Bonus_Settings (key, amount, description) VALUES (:k, :a, '')");
        ins.bindValue(":k", key);
        ins.bindValue(":a", amount);
        if (!ins.exec()) {
            qWarning() << "setBonusAmount insert error:" << ins.lastError().text();
            return false;
        }
    }
    return true;
}

bool UserManager::getAllBonusSettings(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec("SELECT key, amount, description FROM Bonus_Settings ORDER BY key")) {
        qWarning() << "getAllBonusSettings error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    while (q.next()) {
        QJsonObject o;
        o["key"]         = q.value(0).toString();
        o["amount"]      = q.value(1).toInt();
        o["description"] = q.value(2).toString();
        arr.append(o);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

// Награда за уникальное (за день) блюдо в рационе. Возвращает true, если запрос
// корректно обработан. kachballs_awarded = 0, если этот рецепт уже учитывался сегодня.
bool UserManager::addDishToDiet(int user_id, const QString &recipe_name,
                                const QString &date, int &kachballs_awarded)
{
    kachballs_awarded = 0;
    if (!db.isOpen() || user_id <= 0 || recipe_name.trimmed().isEmpty() || date.isEmpty())
        return false;

    QSqlQuery chk(db);
    chk.prepare("SELECT 1 FROM Diet_Reward_Log "
                "WHERE user_id=:uid AND date=:d AND recipe_name=:n");
    chk.bindValue(":uid", user_id);
    chk.bindValue(":d",   date);
    chk.bindValue(":n",   recipe_name);
    if (chk.exec() && chk.next()) {
        // уже начисляли за это блюдо сегодня — не повторяем
        return true;
    }

    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Diet_Reward_Log (user_id, date, recipe_name) "
                "VALUES (:uid, :d, :n)");
    ins.bindValue(":uid", user_id);
    ins.bindValue(":d",   date);
    ins.bindValue(":n",   recipe_name);
    if (!ins.exec()) {
        qWarning() << "addDishToDiet insert error:" << ins.lastError().text();
        return false;
    }

    int reward = getBonusAmount("dish_added", 5);
    if (reward > 0 && addKachballs(user_id, reward)) {
        kachballs_awarded = reward;
        qDebug() << "💰 Diet dish reward +" << reward << "for user" << user_id
                 << "recipe:" << recipe_name;
    }
    return true;
}

bool UserManager::deleteRecipe(int recipe_id)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in deleteRecipe";
        return false;
    }

    db.transaction();

    QSqlQuery q(db);
    q.prepare("DELETE FROM Recipe_Steps WHERE recipe_id = :rid");
    q.bindValue(":rid", recipe_id);
    q.exec();

    q.prepare("DELETE FROM Recipe_Ingredients WHERE recipe_id = :rid");
    q.bindValue(":rid", recipe_id);
    q.exec();

    q.prepare("DELETE FROM Recipes WHERE id = :rid");
    q.bindValue(":rid", recipe_id);
    if (!q.exec()) {
        db.rollback();
        qWarning() << "deleteRecipe error:" << q.lastError().text();
        return false;
    }

    db.commit();
    qDebug() << "deleteRecipe OK: id=" << recipe_id;
    return true;
}

bool UserManager::deleteReview(int review_id, int recipe_id)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in deleteReview";
        return false;
    }

    QSqlQuery q(db);
    q.prepare("DELETE FROM Recipe_Reviews WHERE id = :rid");
    q.bindValue(":rid", review_id);
    if (!q.exec()) {
        qWarning() << "deleteReview error:" << q.lastError().text();
        return false;
    }

    // Пересчитать средний рейтинг рецепта
    QSqlQuery upd(db);
    upd.prepare("UPDATE Recipes SET rating = "
                "COALESCE((SELECT ROUND(AVG(rating), 1) FROM Recipe_Reviews WHERE recipe_id = :rid), 0) "
                "WHERE id = :rid2");
    upd.bindValue(":rid", recipe_id);
    upd.bindValue(":rid2", recipe_id);
    upd.exec();

    qDebug() << "deleteReview OK: reviewId=" << review_id << "recipeId=" << recipe_id;
    return true;
}

bool UserManager::getUserWorkoutInfo(int user_id, QString &jsonData)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in getUserWorkoutInfo";
        return false;
    }

    QJsonObject obj;

    // Получаем PlanTrainning из USERS
    QSqlQuery q(db);
    q.prepare("SELECT PlanTrainning FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) {
        qWarning() << "getUserWorkoutInfo: user not found" << user_id;
        return false;
    }
    int planId = q.value(0).toInt();
    obj["planId"] = planId;

    // Название плана
    QString planName = "Нет плана";
    if (planId == 1) planName = "Тренажёрный зал с нуля";
    else if (planId == 2) planName = "Тренажёрный зал любитель";
    else if (planId == 3) planName = "Песочные часы";
    else if (planId == 4) planName = "Ягодицы без ног";
    else if (planId == 5) planName = "Песочные часы PRO";
    else if (planId == 7) planName = "Учимся подтягиваться";
    obj["planName"] = planName;

    // Получаем прогресс из Training_Week_Progress
    QSqlQuery wp(db);
    wp.prepare("SELECT current_week, workout1_done, workout2_done, workout3_done, "
               "week_completed, plan_start_date FROM Training_Week_Progress WHERE user_id = :uid");
    wp.bindValue(":uid", user_id);
    if (wp.exec() && wp.next()) {
        obj["currentWeek"]   = wp.value(0).toInt();
        obj["workout1Done"]  = wp.value(1).toInt();
        obj["workout2Done"]  = wp.value(2).toInt();
        obj["workout3Done"]  = wp.value(3).toInt();
        obj["weekCompleted"] = wp.value(4).toInt();
        obj["planStartDate"] = wp.value(5).toString();
    } else {
        obj["currentWeek"]   = 0;
        obj["workout1Done"]  = 0;
        obj["workout2Done"]  = 0;
        obj["workout3Done"]  = 0;
        obj["weekCompleted"] = 0;
        obj["planStartDate"] = "";
    }

    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qDebug() << "getUserWorkoutInfo OK: userId=" << user_id << "plan=" << planId;
    return true;
}

// ==================== СТАТИСТИКА ТРЕНИРОВОК ====================

bool UserManager::computeAndSaveWeeklyStats(int user_id)
{
    if (!db.isOpen()) return false;

    // Получаем текущую неделю
    QSqlQuery wq(db);
    wq.prepare("SELECT current_week FROM Training_Week_Progress WHERE user_id = :uid");
    wq.bindValue(":uid", user_id);
    if (!wq.exec() || !wq.next()) return false;
    int week_num = wq.value(0).toInt();

    // Список всех упражнений и их таблиц
    QList<QPair<QString,QString>> exercises = {
        {"Вертикальная тяга",         "Save_Training_Vertical_Thrust_P1"},
        {"Ягодичный мост",            "Save_Training_Buttock_Bridge_P1"},
        {"Жим от груди",              "Save_Training_Chest_Press_P1"},
        {"Жим одной ногой",           "Save_Training_One_Leg_Bench_Press_P1"},
        {"Горизонтальная тяга",       "Save_Training_Horizontal_Thrust_P1"},
        {"Пресс на коврике",          "Save_Training_Press_On_Mat_P1"},
        {"Пресс на мячике",           "Save_Training_Press_On_Ball_P1"},
        {"Приседания с гирей",        "Save_Training_Kettlebell_Squat_P1"},
        {"Румынская тяга",            "Save_Training_Romanian_Deadlift_P1"},
        {"Отжимания с колен",         "Save_Training_Knee_Pushup_P1"},
        {"Отведения бедра сидя",      "Save_Training_Hip_Abduction_P1"},
        {"Разгибание на трицепс",     "Save_Training_Tricep_Extension_P1"},
        {"Подтягивания на тренажере", "Save_Training_Assisted_Pullup_P1"},
        {"Болгарские выпады",         "Save_Training_Bulgarian_Split_Squat_P1"},
        {"Жим наверх в тренажере",    "Save_Training_Shoulder_Press_P1"},
        {"Махи кроссовер",            "Save_Training_Cable_Fly_P1"},
        {"Экстензия",                 "Save_Training_Leg_Extension_P1"},
        {"Планка",                    "Save_Training_Plank_P1"},
        {"Сведение рук в тренажере",  "Save_Training_Chest_Fly_P1"}
    };

    for (const auto &ex : exercises) {
        const QString &exName = ex.first;
        const QString &tableName = ex.second;

        QSqlQuery tq(db);
        tq.prepare(QString("SELECT Approach1, Approach2, Approach3 FROM \"%1\" WHERE user_id = :uid").arg(tableName));
        tq.bindValue(":uid", user_id);
        if (!tq.exec() || !tq.next()) continue;

        float a1 = tq.value(0).toFloat();
        float a2 = tq.value(1).toFloat();
        float a3 = tq.value(2).toFloat();
        float avg = (a1 + a2 + a3) / 3.0f;

        QSqlQuery uq(db);
        uq.prepare(
            "INSERT INTO Workout_Weekly_Stats (user_id, week_num, exercise_name, avg_weight) "
            "VALUES (:uid, :week, :name, :avg) "
            "ON CONFLICT(user_id, week_num, exercise_name) DO UPDATE SET avg_weight = :avg2");
        uq.bindValue(":uid",  user_id);
        uq.bindValue(":week", week_num);
        uq.bindValue(":name", exName);
        uq.bindValue(":avg",  avg);
        uq.bindValue(":avg2", avg);
        uq.exec();
    }

    qDebug() << "computeAndSaveWeeklyStats OK: userId=" << user_id << "week=" << week_num;
    return true;
}

bool UserManager::getWorkoutStats(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;

    // 1. Недельные итоги (суммарный вес по всем упражнениям за каждую неделю)
    QSqlQuery wq(db);
    wq.prepare(
        "SELECT week_num, SUM(avg_weight) as total "
        "FROM Workout_Weekly_Stats WHERE user_id = :uid "
        "GROUP BY week_num ORDER BY week_num ASC");
    wq.bindValue(":uid", user_id);

    QJsonArray weeklyTotals;
    if (wq.exec()) {
        while (wq.next()) {
            QJsonObject obj;
            obj["week"]  = wq.value(0).toInt();
            obj["total"] = qRound(wq.value(1).toDouble());
            weeklyTotals.append(obj);
        }
    }

    // 2. Топ-5 упражнений: сравниваем последнюю неделю с позапрошлой
    // Получаем список уникальных недель
    QSqlQuery wkq(db);
    wkq.prepare("SELECT DISTINCT week_num FROM Workout_Weekly_Stats WHERE user_id = :uid ORDER BY week_num DESC");
    wkq.bindValue(":uid", user_id);
    wkq.exec();

    QList<int> weeks;
    while (wkq.next()) weeks.append(wkq.value(0).toInt());

    QJsonArray top5;
    if (weeks.size() >= 1) {
        int latestWeek = weeks.at(0);
        int prevWeek   = (weeks.size() >= 2) ? weeks.at(1) : -1;

        // Получаем веса за последнюю неделю
        QSqlQuery lq(db);
        lq.prepare("SELECT exercise_name, avg_weight FROM Workout_Weekly_Stats WHERE user_id = :uid AND week_num = :w");
        lq.bindValue(":uid", user_id);
        lq.bindValue(":w", latestWeek);
        lq.exec();

        QMap<QString, float> latestWeights;
        while (lq.next()) latestWeights[lq.value(0).toString()] = lq.value(1).toFloat();

        QMap<QString, float> prevWeights;
        if (prevWeek >= 0) {
            QSqlQuery pq(db);
            pq.prepare("SELECT exercise_name, avg_weight FROM Workout_Weekly_Stats WHERE user_id = :uid AND week_num = :w");
            pq.bindValue(":uid", user_id);
            pq.bindValue(":w", prevWeek);
            pq.exec();
            while (pq.next()) prevWeights[pq.value(0).toString()] = pq.value(1).toFloat();
        }

        // Вычисляем изменения и сортируем по abs(delta)
        struct ExStat { QString name; float delta; };
        QList<ExStat> stats;
        for (auto it = latestWeights.begin(); it != latestWeights.end(); ++it) {
            float prev = prevWeights.value(it.key(), 0.0f);
            float delta = it.value() - prev;
            if (prevWeek < 0) delta = it.value(); // первая неделя — просто текущий вес
            stats.append({it.key(), delta});
        }
        std::sort(stats.begin(), stats.end(), [](const ExStat &a, const ExStat &b){
            return qAbs(a.delta) > qAbs(b.delta);
        });

        int count = 0;
        for (const auto &s : stats) {
            if (count++ >= 5) break;
            QJsonObject obj;
            obj["name"]     = s.name;
            obj["delta"]    = qRound(s.delta * 10.0f) / 10.0;
            obj["positive"] = (s.delta >= 0);
            top5.append(obj);
        }
    }

    QJsonObject result;
    result["weekly_totals"] = weeklyTotals;
    result["top5"] = top5;
    jsonData = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    qDebug() << "getWorkoutStats OK: userId=" << user_id;
    return true;
}

bool UserManager::clearWorkoutStats(int user_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Workout_Weekly_Stats WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    bool ok = q.exec();
    qDebug() << "clearWorkoutStats:" << (ok ? "OK" : "FAILED") << "userId=" << user_id;
    return ok;
}

bool UserManager::adminResetUserPlan(int user_id)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in adminResetUserPlan";
        return false;
    }

    // Сбрасываем PlanTrainning на 0
    QSqlQuery q1(db);
    q1.prepare("UPDATE USERS SET PlanTrainning = 0 WHERE user_id = :uid");
    q1.bindValue(":uid", user_id);
    if (!q1.exec()) {
        qWarning() << "adminResetUserPlan: failed to reset PlanTrainning" << q1.lastError().text();
        return false;
    }

    // Сбрасываем статистику тренировок
    clearWorkoutStats(user_id);

    // Сбрасываем прогресс тренировок
    QSqlQuery q2(db);
    q2.prepare("UPDATE Training_Week_Progress SET current_week = 1, "
               "workout1_done = 0, workout2_done = 0, workout3_done = 0, "
               "w1_date = '', w2_date = '', w3_date = '', "
               "week_completed = 0, plan_start_date = '' "
               "WHERE user_id = :uid");
    q2.bindValue(":uid", user_id);
    q2.exec(); // может не быть записи — это нормально

    qDebug() << "adminResetUserPlan OK: userId=" << user_id;
    return true;
}

// ==================== ДРУЗЬЯ ====================

bool UserManager::getUserFriendId(int user_id, int &friendId)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT userFriendID FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;
    friendId = q.value(0).toInt();
    return true;
}

bool UserManager::searchUserByFriendId(int myUserId, int targetFriendId, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(
        "SELECT user_id, first_name, last_name, "
        "CASE WHEN birth_date IS NULL OR birth_date = '' THEN 0 "
        "     ELSE CAST((julianday('now') - julianday(birth_date)) / 365.25 AS INTEGER) END AS age, "
        "Goal, userFriendID, imageAvatar "
        "FROM USERS "
        "WHERE userFriendID = :fid AND user_id != :myId");
    q.bindValue(":fid", targetFriendId);
    q.bindValue(":myId", myUserId);
    if (!q.exec()) {
        qWarning() << "searchUserByFriendId error:" << q.lastError().text();
        return false;
    }
    if (!q.next()) {
        jsonData = "";
        return true; // не нашли — не ошибка
    }
    QJsonObject obj;
    obj["userId"]   = q.value(0).toInt();
    obj["firstName"]= q.value(1).toString();
    obj["lastName"] = q.value(2).toString();
    obj["age"]      = q.value(3).toInt();
    obj["goal"]     = q.value(4).toString();
    obj["friendId"] = q.value(5).toInt();
    QByteArray avatarBlob = q.value(6).toByteArray();
    if (!avatarBlob.isEmpty())
        obj["avatar"] = QString::fromLatin1(avatarBlob.toBase64());
    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::sendFriendRequest(int fromUserId, int targetFriendId, QString &errorMessage, int &toUserId)
{
    if (!db.isOpen()) { errorMessage = "db_error"; return false; }

    // Ищем user_id по targetFriendId
    QSqlQuery find(db);
    find.prepare("SELECT user_id FROM USERS WHERE userFriendID = :fid");
    find.bindValue(":fid", targetFriendId);
    if (!find.exec() || !find.next()) {
        errorMessage = "not_found";
        toUserId = -1;
        return false;
    }
    toUserId = find.value(0).toInt();

    if (toUserId == fromUserId) { errorMessage = "self"; return false; }

    // Проверяем: уже друзья или запрос уже есть
    QSqlQuery chk(db);
    chk.prepare("SELECT status FROM Friends WHERE "
                "(from_user_id = :a AND to_user_id = :b) OR "
                "(from_user_id = :b2 AND to_user_id = :a2)");
    chk.bindValue(":a", fromUserId);
    chk.bindValue(":b", toUserId);
    chk.bindValue(":b2", toUserId);
    chk.bindValue(":a2", fromUserId);
    if (chk.exec() && chk.next()) {
        QString st = chk.value(0).toString();
        if (st == "accepted") { errorMessage = "already_friends"; return false; }
        if (st == "pending")  { errorMessage = "already_sent";    return false; }
    }

    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Friends (from_user_id, to_user_id, status) VALUES (:a, :b, 'pending')");
    ins.bindValue(":a", fromUserId);
    ins.bindValue(":b", toUserId);
    if (!ins.exec()) {
        errorMessage = ins.lastError().text();
        return false;
    }
    qDebug() << "Friend request sent from" << fromUserId << "to" << toUserId;
    return true;
}

bool UserManager::getIncomingFriendRequests(int userId, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(
        "SELECT f.id, u.user_id, u.first_name, u.last_name, "
        "CASE WHEN u.birth_date IS NULL OR u.birth_date = '' THEN 0 "
        "     ELSE CAST((julianday('now') - julianday(u.birth_date)) / 365.25 AS INTEGER) END AS age, "
        "u.Goal, u.userFriendID, u.imageAvatar "
        "FROM Friends f "
        "JOIN USERS u ON u.user_id = f.from_user_id "
        "WHERE f.to_user_id = :uid AND f.status = 'pending' "
        "ORDER BY f.created_at DESC");
    q.bindValue(":uid", userId);
    if (!q.exec()) {
        qWarning() << "getIncomingFriendRequests error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["requestId"] = q.value(0).toInt();
        obj["userId"]    = q.value(1).toInt();
        obj["firstName"] = q.value(2).toString();
        obj["lastName"]  = q.value(3).toString();
        obj["age"]       = q.value(4).toInt();
        obj["goal"]      = q.value(5).toString();
        obj["friendId"]  = q.value(6).toInt();
        QByteArray avatarBlob = q.value(7).toByteArray();
        if (!avatarBlob.isEmpty())
            obj["avatar"] = QString::fromLatin1(avatarBlob.toBase64());
        arr.append(obj);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

bool UserManager::acceptFriendRequest(int myUserId, int requestId)
{
    if (!db.isOpen()) return false;

    // Находим запрос
    QSqlQuery find(db);
    find.prepare("SELECT from_user_id FROM Friends WHERE id = :rid AND to_user_id = :uid AND status = 'pending'");
    find.bindValue(":rid", requestId);
    find.bindValue(":uid", myUserId);
    if (!find.exec() || !find.next()) return false;
    int fromUserId = find.value(0).toInt();

    // Обновляем оригинальный pending → accepted
    QSqlQuery upd(db);
    upd.prepare("UPDATE Friends SET status = 'accepted' WHERE id = :rid");
    upd.bindValue(":rid", requestId);
    if (!upd.exec()) return false;

    // Добавляем обратную запись (myUserId → fromUserId)
    QSqlQuery ins(db);
    ins.prepare("INSERT OR IGNORE INTO Friends (from_user_id, to_user_id, status) VALUES (:a, :b, 'accepted')");
    ins.bindValue(":a", myUserId);
    ins.bindValue(":b", fromUserId);
    ins.exec();

    qDebug() << "Friend request accepted:" << requestId << "between" << myUserId << "and" << fromUserId;
    return true;
}

bool UserManager::declineFriendRequest(int myUserId, int requestId)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Friends WHERE id = :rid AND to_user_id = :uid AND status = 'pending'");
    q.bindValue(":rid", requestId);
    q.bindValue(":uid", myUserId);
    return q.exec() && q.numRowsAffected() > 0;
}

bool UserManager::getFriends(int userId, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(
        "SELECT u.user_id, u.first_name, u.last_name, "
        "CASE WHEN u.birth_date IS NULL OR u.birth_date = '' THEN 0 "
        "     ELSE CAST((julianday('now') - julianday(u.birth_date)) / 365.25 AS INTEGER) END AS age, "
        "u.Goal, u.userFriendID, u.imageAvatar, "
        "COALESCE(u.kachballs, 0) "
        "FROM Friends f "
        "JOIN USERS u ON u.user_id = f.to_user_id "
        "WHERE f.from_user_id = :uid AND f.status = 'accepted' "
        "ORDER BY u.first_name, u.last_name");
    q.bindValue(":uid", userId);
    if (!q.exec()) {
        qWarning() << "getFriends error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["userId"]    = q.value(0).toInt();
        obj["firstName"] = q.value(1).toString();
        obj["lastName"]  = q.value(2).toString();
        obj["age"]       = q.value(3).toInt();
        obj["goal"]      = q.value(4).toString();
        obj["friendId"]  = q.value(5).toInt();
        QByteArray avatarBlob = q.value(6).toByteArray();
        if (!avatarBlob.isEmpty())
            obj["avatar"] = QString::fromLatin1(avatarBlob.toBase64());
        obj["kachballs"] = q.value(7).toInt();
        arr.append(obj);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "getFriends OK: user" << userId << "count=" << arr.size();
    return true;
}

bool UserManager::removeFriend(int myUserId, int friendUserId)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Friends WHERE "
              "(from_user_id = :a AND to_user_id = :b) OR "
              "(from_user_id = :b2 AND to_user_id = :a2)");
    q.bindValue(":a",  myUserId);
    q.bindValue(":b",  friendUserId);
    q.bindValue(":b2", friendUserId);
    q.bindValue(":a2", myUserId);
    return q.exec();
}

// ==================== ДОСТИЖЕНИЯ И КАЧБАЛЛЫ ====================

// ---------------------------------------------------------------------------
// Получить список достижений пользователя (полученные + все доступные)
// ---------------------------------------------------------------------------
bool UserManager::getUserAchievements(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare(
        "SELECT a.id, a.key, a.name, a.description, a.category, a.reward, a.threshold, "
        "       ua.earned_at "
        "FROM Achievements a "
        "LEFT JOIN User_Achievements ua ON ua.achievement_id = a.id AND ua.user_id = :uid "
        "ORDER BY a.category, a.id");
    q.bindValue(":uid", user_id);
    if (!q.exec()) {
        qWarning() << "getUserAchievements error:" << q.lastError().text();
        return false;
    }

    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["id"]          = q.value(0).toInt();
        obj["key"]         = q.value(1).toString();
        obj["name"]        = q.value(2).toString();
        obj["description"] = q.value(3).toString();
        obj["category"]    = q.value(4).toString();
        obj["reward"]      = q.value(5).toInt();
        obj["threshold"]   = q.value(6).toInt();
        obj["earned"]      = !q.value(7).isNull();
        obj["earnedAt"]    = q.value(7).toString();
        arr.append(obj);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return true;
}

// ---------------------------------------------------------------------------
// Получить количество качбаллов
// ---------------------------------------------------------------------------
bool UserManager::getUserKachballs(int user_id, int &kachballs)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT COALESCE(kachballs, 0) FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) return false;
    kachballs = q.value(0).toInt();
    return true;
}

// ---------------------------------------------------------------------------
// Проверить и выдать новые достижения. Возвращает JSON-массив НОВЫХ.
// ---------------------------------------------------------------------------
bool UserManager::checkAndGrantAchievements(int user_id, QString &newAchievementsJson)
{
    if (!db.isOpen()) return false;

    // Собираем статистику пользователя
    struct Stat {
        int totalWorkouts   = 0;
        int weeksCompleted  = 0;
        int planCompleted   = 0;   // 0 or 1
        int friendsCount    = 0;
        int reviewsCount    = 0;
        int measurementsCount = 0;
        int nutritionDays   = 0;
        int nutritionStreak = 0;
    } s;

    // --- Общее число завершённых тренировок (workout1+2+3 по всем неделям) ---
    {
        QSqlQuery q(db);
        q.prepare(
            "SELECT COALESCE(workout1_done,0) + COALESCE(workout2_done,0) + COALESCE(workout3_done,0), "
            "       current_week, week_completed "
            "FROM Training_Week_Progress WHERE user_id = :uid");
        q.bindValue(":uid", user_id);
        if (q.exec() && q.next()) {
            int doneThisWeek = q.value(0).toInt();
            int currentWeek  = q.value(1).toInt();
            int weekDone     = q.value(2).toInt();

            // Завершённые предыдущие недели * 3 + текущая
            s.weeksCompleted  = currentWeek - 1 + (weekDone ? 1 : 0);
            s.totalWorkouts   = (currentWeek - 1) * 3 + doneThisWeek;
        }
    }

    // --- Статистика по weekly stats (более точный подсчёт) ---
    {
        QSqlQuery q(db);
        q.prepare("SELECT COUNT(DISTINCT week_num) FROM Workout_Weekly_Stats WHERE user_id = :uid");
        q.bindValue(":uid", user_id);
        if (q.exec() && q.next()) {
            int statsWeeks = q.value(0).toInt();
            if (statsWeeks > s.weeksCompleted)
                s.weeksCompleted = statsWeeks;
        }
    }

    // --- План завершён? ---
    {
        QSqlQuery q(db);
        q.prepare("SELECT plan_start_date FROM Training_Week_Progress WHERE user_id = :uid");
        q.bindValue(":uid", user_id);
        if (q.exec() && q.next()) {
            QString psd = q.value(0).toString();
            if (!psd.isEmpty()) {
                QDateTime start = QDateTime::fromString(psd, Qt::ISODate);
                if (start.isValid() && start.daysTo(QDateTime::currentDateTime()) >= 56)
                    s.planCompleted = 1;
            }
        }
    }

    // --- Друзья ---
    {
        QSqlQuery q(db);
        q.prepare("SELECT COUNT(*) FROM Friends WHERE from_user_id = :uid AND status = 'accepted'");
        q.bindValue(":uid", user_id);
        if (q.exec() && q.next())
            s.friendsCount = q.value(0).toInt();
    }

    // --- Отзывы ---
    {
        QSqlQuery q(db);
        q.prepare("SELECT COUNT(*) FROM Recipe_Reviews WHERE user_id = :uid");
        q.bindValue(":uid", user_id);
        if (q.exec() && q.next())
            s.reviewsCount = q.value(0).toInt();
    }

    // --- Замеры ---
    {
        QSqlQuery q(db);
        q.prepare("SELECT COUNT(*) FROM UserMeasurements WHERE UserId = :uid");
        q.bindValue(":uid", user_id);
        if (q.exec() && q.next())
            s.measurementsCount = q.value(0).toInt();
    }

    // --- Питание: общее число дней и streak ---
    {
        QSqlQuery q(db);
        q.prepare("SELECT log_date FROM Daily_Nutrition_Log WHERE user_id = :uid ORDER BY log_date DESC");
        q.bindValue(":uid", user_id);
        if (q.exec()) {
            QVector<QDate> dates;
            while (q.next())
                dates.append(QDate::fromString(q.value(0).toString(), "yyyy-MM-dd"));
            s.nutritionDays = dates.size();

            // Считаем streak от сегодня назад
            if (!dates.isEmpty()) {
                s.nutritionStreak = 1;
                for (int i = 1; i < dates.size(); ++i) {
                    if (dates[i-1].toJulianDay() - dates[i].toJulianDay() == 1)
                        s.nutritionStreak++;
                    else
                        break;
                }
            }
        }
    }

    // --- Сопоставляем ключи с порогами ---
    QMap<QString, int> progress;
    progress["first_workout"]     = s.totalWorkouts;
    progress["workouts_10"]       = s.totalWorkouts;
    progress["workouts_50"]       = s.totalWorkouts;
    progress["workouts_100"]      = s.totalWorkouts;
    progress["week_complete"]     = s.weeksCompleted;
    progress["plan_complete"]     = s.planCompleted;
    progress["weeks_4"]           = s.weeksCompleted;
    progress["first_friend"]      = s.friendsCount;
    progress["friends_5"]         = s.friendsCount;
    progress["friends_10"]        = s.friendsCount;
    progress["first_review"]      = s.reviewsCount;
    progress["reviews_5"]         = s.reviewsCount;
    progress["first_measurement"] = s.measurementsCount;
    progress["measurements_10"]   = s.measurementsCount;
    progress["nutrition_7_days"]  = s.nutritionStreak;
    progress["nutrition_30_days"] = s.nutritionStreak;
    progress["first_nutrition"]   = s.nutritionDays;

    // --- Проверяем каждое достижение ---
    QJsonArray newArr;
    QSqlQuery allAch(db);
    allAch.exec("SELECT id, key, name, description, category, reward, threshold FROM Achievements");
    while (allAch.next()) {
        int    achId     = allAch.value(0).toInt();
        QString key      = allAch.value(1).toString();
        QString name     = allAch.value(2).toString();
        QString desc     = allAch.value(3).toString();
        QString category = allAch.value(4).toString();
        int    reward    = allAch.value(5).toInt();
        int    threshold = allAch.value(6).toInt();

        // Уже получено?
        QSqlQuery chk(db);
        chk.prepare("SELECT 1 FROM User_Achievements WHERE user_id = :uid AND achievement_id = :aid");
        chk.bindValue(":uid", user_id);
        chk.bindValue(":aid", achId);
        if (chk.exec() && chk.next())
            continue;  // уже есть

        // Хватает прогресса?
        int userProgress = progress.value(key, 0);
        if (userProgress < threshold)
            continue;  // не дотянул

        // Выдаём!
        QSqlQuery grant(db);
        grant.prepare("INSERT OR IGNORE INTO User_Achievements (user_id, achievement_id) VALUES (:uid, :aid)");
        grant.bindValue(":uid", user_id);
        grant.bindValue(":aid", achId);
        if (!grant.exec()) {
            qWarning() << "Failed to grant achievement" << key << ":" << grant.lastError().text();
            continue;
        }

        // Начисляем качбаллы
        QSqlQuery addBalls(db);
        addBalls.prepare("UPDATE USERS SET kachballs = COALESCE(kachballs, 0) + :reward WHERE user_id = :uid");
        addBalls.bindValue(":reward", reward);
        addBalls.bindValue(":uid",    user_id);
        addBalls.exec();

        QJsonObject obj;
        obj["id"]          = achId;
        obj["key"]         = key;
        obj["name"]        = name;
        obj["description"] = desc;
        obj["category"]    = category;
        obj["reward"]      = reward;
        newArr.append(obj);

        qDebug() << "🏆 Achievement granted:" << key << "to user" << user_id << "(+" << reward << " kachballs)";
    }

    newAchievementsJson = QString::fromUtf8(QJsonDocument(newArr).toJson(QJsonDocument::Compact));
    return true;
}

// ---------------------------------------------------------------------------
// Получить/создать еженедельные миссии
// ---------------------------------------------------------------------------
bool UserManager::getWeeklyMissions(int user_id, int year, int week, QString &jsonData)
{
    if (!db.isOpen()) return false;

    // Убедимся, что запись существует
    QSqlQuery ins(db);
    ins.prepare("INSERT OR IGNORE INTO Weekly_Missions (user_id, year, week_num) "
                "VALUES (:uid, :yr, :wk)");
    ins.bindValue(":uid", user_id);
    ins.bindValue(":yr",  year);
    ins.bindValue(":wk",  week);
    ins.exec();

    QSqlQuery q(db);
    q.prepare("SELECT steps_done, workouts_done, nutrition_done, "
              "       steps_rewarded, workouts_rewarded, nutrition_rewarded, full_bonus_rewarded "
              "FROM Weekly_Missions WHERE user_id=:uid AND year=:yr AND week_num=:wk");
    q.bindValue(":uid", user_id);
    q.bindValue(":yr",  year);
    q.bindValue(":wk",  week);
    if (!q.exec() || !q.next()) return false;

    QJsonObject obj;
    obj["year"]               = year;
    obj["week"]               = week;
    obj["steps_done"]         = q.value(0).toInt() == 1;
    obj["workouts_done"]      = q.value(1).toInt() == 1;
    obj["nutrition_done"]     = q.value(2).toInt() == 1;
    obj["steps_rewarded"]     = q.value(3).toInt() == 1;
    obj["workouts_rewarded"]  = q.value(4).toInt() == 1;
    obj["nutrition_rewarded"] = q.value(5).toInt() == 1;
    obj["full_bonus_rewarded"]= q.value(6).toInt() == 1;
    obj["kachballs_awarded"]  = 0;  // не начислялось при этом запросе

    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}

// ---------------------------------------------------------------------------
// Обновить миссии и начислить кач-баллы за новые выполнения
// ---------------------------------------------------------------------------
bool UserManager::updateWeeklyMissions(int user_id, int year, int week,
                                       bool steps_done, bool workouts_done, bool nutrition_done,
                                       int &kachballs_awarded, QString &jsonData)
{
    if (!db.isOpen()) return false;
    kachballs_awarded = 0;

    // Создаём запись если нет
    QSqlQuery ins(db);
    ins.prepare("INSERT OR IGNORE INTO Weekly_Missions (user_id, year, week_num) "
                "VALUES (:uid, :yr, :wk)");
    ins.bindValue(":uid", user_id);
    ins.bindValue(":yr",  year);
    ins.bindValue(":wk",  week);
    ins.exec();

    // Читаем текущее состояние
    QSqlQuery cur(db);
    cur.prepare("SELECT steps_done, workouts_done, nutrition_done, "
                "       steps_rewarded, workouts_rewarded, nutrition_rewarded, full_bonus_rewarded "
                "FROM Weekly_Missions WHERE user_id=:uid AND year=:yr AND week_num=:wk");
    cur.bindValue(":uid", user_id);
    cur.bindValue(":yr",  year);
    cur.bindValue(":wk",  week);
    if (!cur.exec() || !cur.next()) return false;

    bool cur_steps_done      = cur.value(0).toInt() == 1;
    bool cur_workouts_done   = cur.value(1).toInt() == 1;
    bool cur_nutrition_done  = cur.value(2).toInt() == 1;
    bool steps_rewarded      = cur.value(3).toInt() == 1;
    bool workouts_rewarded   = cur.value(4).toInt() == 1;
    bool nutrition_rewarded  = cur.value(5).toInt() == 1;
    bool full_bonus_rewarded = cur.value(6).toInt() == 1;

    // Новые значения (не откатываем уже выполненное)
    bool new_steps     = cur_steps_done     || steps_done;
    bool new_workouts  = cur_workouts_done  || workouts_done;
    bool new_nutrition = cur_nutrition_done || nutrition_done;

    // Начисляем баллы за новые выполнения
    int earned = 0;
    int new_steps_rew = steps_rewarded     ? 1 : 0;
    int new_work_rew  = workouts_rewarded  ? 1 : 0;
    int new_nutr_rew  = nutrition_rewarded ? 1 : 0;
    int new_full_rew  = full_bonus_rewarded ? 1 : 0;

    if (new_steps && !steps_rewarded) {
        earned += 10;
        new_steps_rew = 1;
    }
    if (new_workouts && !workouts_rewarded) {
        earned += 20;
        new_work_rew = 1;
    }
    if (new_nutrition && !nutrition_rewarded) {
        earned += 10;
        new_nutr_rew = 1;
    }
    if (new_steps && new_workouts && new_nutrition && !full_bonus_rewarded) {
        earned += 10;   // бонус за все три
        new_full_rew = 1;
    }

    // Сохраняем
    QSqlQuery upd(db);
    upd.prepare("UPDATE Weekly_Missions SET "
                "steps_done=:sd, workouts_done=:wd, nutrition_done=:nd, "
                "steps_rewarded=:sr, workouts_rewarded=:wr, nutrition_rewarded=:nr, full_bonus_rewarded=:fr "
                "WHERE user_id=:uid AND year=:yr AND week_num=:wk");
    upd.bindValue(":sd", new_steps     ? 1 : 0);
    upd.bindValue(":wd", new_workouts  ? 1 : 0);
    upd.bindValue(":nd", new_nutrition ? 1 : 0);
    upd.bindValue(":sr", new_steps_rew);
    upd.bindValue(":wr", new_work_rew);
    upd.bindValue(":nr", new_nutr_rew);
    upd.bindValue(":fr", new_full_rew);
    upd.bindValue(":uid", user_id);
    upd.bindValue(":yr",  year);
    upd.bindValue(":wk",  week);
    if (!upd.exec()) return false;

    // Начисляем кач-баллы
    if (earned > 0) {
        QSqlQuery addB(db);
        addB.prepare("UPDATE USERS SET kachballs = COALESCE(kachballs,0) + :pts WHERE user_id = :uid");
        addB.bindValue(":pts", earned);
        addB.bindValue(":uid", user_id);
        addB.exec();
        qDebug() << "💰 Weekly missions kachballs +" << earned << "for user" << user_id;
    }

    kachballs_awarded = earned;

    // Возвращаем актуальный JSON
    QJsonObject obj;
    obj["year"]                = year;
    obj["week"]                = week;
    obj["steps_done"]          = new_steps;
    obj["workouts_done"]       = new_workouts;
    obj["nutrition_done"]      = new_nutrition;
    obj["steps_rewarded"]      = new_steps_rew == 1;
    obj["workouts_rewarded"]   = new_work_rew  == 1;
    obj["nutrition_rewarded"]  = new_nutr_rew  == 1;
    obj["full_bonus_rewarded"] = new_full_rew  == 1;
    obj["kachballs_awarded"]   = earned;

    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}

// =====================================================================
// ✅ PLAN 2 — 5 новых упражнений
// =====================================================================

bool UserManager::saveTrainingHipAbductionLeanP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingHipAbductionLeanP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Hip_Abduction_Lean_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE Save_Training_Hip_Abduction_Lean_P2 SET Approach1=:a1,Approach2=:a2,Approach3=:a3 WHERE user_id=:uid");
        upd.bindValue(":a1", approach1); upd.bindValue(":a2", approach2); upd.bindValue(":a3", approach3); upd.bindValue(":uid", user_id);
        return upd.exec();
    }
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Save_Training_Hip_Abduction_Lean_P2 (user_id,Approach1,Approach2,Approach3) VALUES(:uid,:a1,:a2,:a3)");
    ins.bindValue(":uid", user_id); ins.bindValue(":a1", approach1); ins.bindValue(":a2", approach2); ins.bindValue(":a3", approach3);
    return ins.exec();
}

bool UserManager::GetSaveTrainingHipAbductionLeanP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingHipAbductionLeanP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT Approach1,Approach2,Approach3 FROM Save_Training_Hip_Abduction_Lean_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) { approach1=query.value(0).toFloat(); approach2=query.value(1).toFloat(); approach3=query.value(2).toFloat(); return true; }
    return false;
}

bool UserManager::saveTrainingPushupP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingPushupP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Pushup_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE Save_Training_Pushup_P2 SET Approach1=:a1,Approach2=:a2,Approach3=:a3 WHERE user_id=:uid");
        upd.bindValue(":a1", approach1); upd.bindValue(":a2", approach2); upd.bindValue(":a3", approach3); upd.bindValue(":uid", user_id);
        return upd.exec();
    }
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Save_Training_Pushup_P2 (user_id,Approach1,Approach2,Approach3) VALUES(:uid,:a1,:a2,:a3)");
    ins.bindValue(":uid", user_id); ins.bindValue(":a1", approach1); ins.bindValue(":a2", approach2); ins.bindValue(":a3", approach3);
    return ins.exec();
}

bool UserManager::GetSaveTrainingPushupP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingPushupP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT Approach1,Approach2,Approach3 FROM Save_Training_Pushup_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) { approach1=query.value(0).toFloat(); approach2=query.value(1).toFloat(); approach3=query.value(2).toFloat(); return true; }
    return false;
}

bool UserManager::saveTrainingHipAbduction90P2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingHipAbduction90P2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Hip_Abduction_90_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE Save_Training_Hip_Abduction_90_P2 SET Approach1=:a1,Approach2=:a2,Approach3=:a3 WHERE user_id=:uid");
        upd.bindValue(":a1", approach1); upd.bindValue(":a2", approach2); upd.bindValue(":a3", approach3); upd.bindValue(":uid", user_id);
        return upd.exec();
    }
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Save_Training_Hip_Abduction_90_P2 (user_id,Approach1,Approach2,Approach3) VALUES(:uid,:a1,:a2,:a3)");
    ins.bindValue(":uid", user_id); ins.bindValue(":a1", approach1); ins.bindValue(":a2", approach2); ins.bindValue(":a3", approach3);
    return ins.exec();
}

bool UserManager::GetSaveTrainingHipAbduction90P2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingHipAbduction90P2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT Approach1,Approach2,Approach3 FROM Save_Training_Hip_Abduction_90_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) { approach1=query.value(0).toFloat(); approach2=query.value(1).toFloat(); approach3=query.value(2).toFloat(); return true; }
    return false;
}

bool UserManager::saveTrainingPullupP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingPullupP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Pullup_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE Save_Training_Pullup_P2 SET Approach1=:a1,Approach2=:a2,Approach3=:a3 WHERE user_id=:uid");
        upd.bindValue(":a1", approach1); upd.bindValue(":a2", approach2); upd.bindValue(":a3", approach3); upd.bindValue(":uid", user_id);
        return upd.exec();
    }
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Save_Training_Pullup_P2 (user_id,Approach1,Approach2,Approach3) VALUES(:uid,:a1,:a2,:a3)");
    ins.bindValue(":uid", user_id); ins.bindValue(":a1", approach1); ins.bindValue(":a2", approach2); ins.bindValue(":a3", approach3);
    return ins.exec();
}

bool UserManager::GetSaveTrainingPullupP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingPullupP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT Approach1,Approach2,Approach3 FROM Save_Training_Pullup_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) { approach1=query.value(0).toFloat(); approach2=query.value(1).toFloat(); approach3=query.value(2).toFloat(); return true; }
    return false;
}

bool UserManager::saveTrainingLegExtensionFrogP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== saveTrainingLegExtensionFrogP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT user_id FROM Save_Training_Leg_Extension_Frog_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE Save_Training_Leg_Extension_Frog_P2 SET Approach1=:a1,Approach2=:a2,Approach3=:a3 WHERE user_id=:uid");
        upd.bindValue(":a1", approach1); upd.bindValue(":a2", approach2); upd.bindValue(":a3", approach3); upd.bindValue(":uid", user_id);
        return upd.exec();
    }
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Save_Training_Leg_Extension_Frog_P2 (user_id,Approach1,Approach2,Approach3) VALUES(:uid,:a1,:a2,:a3)");
    ins.bindValue(":uid", user_id); ins.bindValue(":a1", approach1); ins.bindValue(":a2", approach2); ins.bindValue(":a3", approach3);
    return ins.exec();
}

bool UserManager::GetSaveTrainingLegExtensionFrogP2(int user_id, float &approach1, float &approach2, float &approach3)
{
    qDebug() << "=== GetSaveTrainingLegExtensionFrogP2 ===" << "User:" << user_id;
    QSqlQuery query(db);
    query.prepare("SELECT Approach1,Approach2,Approach3 FROM Save_Training_Leg_Extension_Frog_P2 WHERE user_id=:user_id");
    query.bindValue(":user_id", user_id);
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; }
    if (query.next()) { approach1=query.value(0).toFloat(); approach2=query.value(1).toFloat(); approach3=query.value(2).toFloat(); return true; }
    return false;
}

// =====================================================================
// ✅ PLAN 3 — 9 новых упражнений
// =====================================================================

#define IMPL_SAVE(FuncName, TableName) \
bool UserManager::FuncName(int user_id, float &approach1, float &approach2, float &approach3) \
{ \
    qDebug() << "===" #FuncName "===" << "User:" << user_id; \
    QSqlQuery query(db); \
    query.prepare("SELECT user_id FROM " TableName " WHERE user_id=:user_id"); \
    query.bindValue(":user_id", user_id); \
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; } \
    if (query.next()) { \
        QSqlQuery upd(db); \
        upd.prepare("UPDATE " TableName " SET Approach1=:a1,Approach2=:a2,Approach3=:a3 WHERE user_id=:uid"); \
        upd.bindValue(":a1", approach1); upd.bindValue(":a2", approach2); upd.bindValue(":a3", approach3); upd.bindValue(":uid", user_id); \
        return upd.exec(); \
    } \
    QSqlQuery ins(db); \
    ins.prepare("INSERT INTO " TableName " (user_id,Approach1,Approach2,Approach3) VALUES(:uid,:a1,:a2,:a3)"); \
    ins.bindValue(":uid", user_id); ins.bindValue(":a1", approach1); ins.bindValue(":a2", approach2); ins.bindValue(":a3", approach3); \
    return ins.exec(); \
}

#define IMPL_LOAD(FuncName, TableName) \
bool UserManager::FuncName(int user_id, float &approach1, float &approach2, float &approach3) \
{ \
    qDebug() << "===" #FuncName "===" << "User:" << user_id; \
    QSqlQuery query(db); \
    query.prepare("SELECT Approach1,Approach2,Approach3 FROM " TableName " WHERE user_id=:user_id"); \
    query.bindValue(":user_id", user_id); \
    if (!query.exec()) { qWarning() << query.lastError().text(); return false; } \
    if (query.next()) { approach1=query.value(0).toFloat(); approach2=query.value(1).toFloat(); approach3=query.value(2).toFloat(); return true; } \
    return false; \
}

// Plan 3
IMPL_SAVE(saveTrainingCraneP3,                      "Save_Training_Crane_P3")
IMPL_LOAD(GetSaveTrainingCraneP3,                   "Save_Training_Crane_P3")

IMPL_SAVE(saveTrainingShoulderSideRaiseSitP3,       "Save_Training_Shoulder_Side_Raise_Sit_P3")
IMPL_LOAD(GetSaveTrainingShoulderSideRaiseSitP3,    "Save_Training_Shoulder_Side_Raise_Sit_P3")

IMPL_SAVE(saveTrainingGoodmorningHackP3,            "Save_Training_Goodmorning_Hack_P3")
IMPL_LOAD(GetSaveTrainingGoodmorningHackP3,         "Save_Training_Goodmorning_Hack_P3")

IMPL_SAVE(saveTrainingShoulderRaiseOneHandBenchP3,  "Save_Training_Shoulder_Raise_One_Hand_Bench_P3")
IMPL_LOAD(GetSaveTrainingShoulderRaiseOneHandBenchP3,"Save_Training_Shoulder_Raise_One_Hand_Bench_P3")

IMPL_SAVE(saveTrainingFroggyStandP3,                "Save_Training_Froggy_Stand_P3")
IMPL_LOAD(GetSaveTrainingFroggyStandP3,             "Save_Training_Froggy_Stand_P3")

IMPL_SAVE(saveTrainingHackSquatP3,                  "Save_Training_Hack_Squat_P3")
IMPL_LOAD(GetSaveTrainingHackSquatP3,               "Save_Training_Hack_Squat_P3")

IMPL_SAVE(saveTrainingPulloverP3,                   "Save_Training_Pullover_P3")
IMPL_LOAD(GetSaveTrainingPulloverP3,                "Save_Training_Pullover_P3")

IMPL_SAVE(saveTrainingCrossoverDiagonalKickP3,      "Save_Training_Crossover_Diagonal_Kick_P3")
IMPL_LOAD(GetSaveTrainingCrossoverDiagonalKickP3,   "Save_Training_Crossover_Diagonal_Kick_P3")

IMPL_SAVE(saveTrainingWallAbductionOneP3,           "Save_Training_Wall_Abduction_One_P3")
IMPL_LOAD(GetSaveTrainingWallAbductionOneP3,        "Save_Training_Wall_Abduction_One_P3")

// =====================================================================
// ✅ PLAN 4 — 6 новых упражнений
// =====================================================================

IMPL_SAVE(saveTrainingCableKickbackP4,              "Save_Training_Cable_Kickback_P4")
IMPL_LOAD(GetSaveTrainingCableKickbackP4,           "Save_Training_Cable_Kickback_P4")

IMPL_SAVE(saveTrainingCrossoverStepUpP4,            "Save_Training_Crossover_Step_Up_P4")
IMPL_LOAD(GetSaveTrainingCrossoverStepUpP4,         "Save_Training_Crossover_Step_Up_P4")

IMPL_SAVE(saveTrainingDipsP4,                       "Save_Training_Dips_P4")
IMPL_LOAD(GetSaveTrainingDipsP4,                    "Save_Training_Dips_P4")

IMPL_SAVE(saveTrainingCableStorkP4,                 "Save_Training_Cable_Stork_P4")
IMPL_LOAD(GetSaveTrainingCableStorkP4,              "Save_Training_Cable_Stork_P4")

IMPL_SAVE(saveTrainingGluteBridgeSingleLegP4,       "Save_Training_Glute_Bridge_Single_Leg_P4")
IMPL_LOAD(GetSaveTrainingGluteBridgeSingleLegP4,    "Save_Training_Glute_Bridge_Single_Leg_P4")

IMPL_SAVE(saveTrainingCableRotationP4,              "Save_Training_Cable_Rotation_P4")
IMPL_LOAD(GetSaveTrainingCableRotationP4,           "Save_Training_Cable_Rotation_P4")

// =====================================================================
// ✅ PLAN 7 — 4 новых упражнения
// =====================================================================

IMPL_SAVE(saveTrainingNegativePullupP7,             "Save_Training_Negative_Pullup_P7")
IMPL_LOAD(GetSaveTrainingNegativePullupP7,          "Save_Training_Negative_Pullup_P7")

IMPL_SAVE(saveTrainingBarbellBicepCurlP7,           "Save_Training_Barbell_Bicep_Curl_P7")
IMPL_LOAD(GetSaveTrainingBarbellBicepCurlP7,        "Save_Training_Barbell_Bicep_Curl_P7")

IMPL_SAVE(saveTrainingLegRaiseElbowP7,              "Save_Training_Leg_Raise_Elbow_P7")
IMPL_LOAD(GetSaveTrainingLegRaiseElbowP7,           "Save_Training_Leg_Raise_Elbow_P7")

IMPL_SAVE(saveTrainingBrachialiscurlP7,             "Save_Training_Brachialis_Curl_P7")
IMPL_LOAD(GetSaveTrainingBrachialiscurlP7,          "Save_Training_Brachialis_Curl_P7")

// =====================================================================
// ✅ ADMIN — Лидерборд, обнуление баллов, геймификация
// =====================================================================

bool UserManager::getLeaderboard(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec("SELECT user_id, first_name, last_name, COALESCE(kachballs,0) AS kb "
                "FROM USERS WHERE COALESCE(kachballs,0) > 0 ORDER BY kb DESC LIMIT 100")) {
        qWarning() << "getLeaderboard error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    int rank = 1;
    while (q.next()) {
        QJsonObject obj;
        obj["rank"]       = rank++;
        obj["user_id"]    = q.value(0).toInt();
        obj["first_name"] = q.value(1).toString();
        obj["last_name"]  = q.value(2).toString();
        obj["kachballs"]  = q.value(3).toInt();
        arr.append(obj);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "getLeaderboard OK: count=" << arr.size();
    return true;
}

bool UserManager::resetAllKachballs()
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec("UPDATE USERS SET kachballs = 0")) {
        qWarning() << "resetAllKachballs error:" << q.lastError().text();
        return false;
    }
    qDebug() << "resetAllKachballs OK: rows=" << q.numRowsAffected();
    return true;
}

bool UserManager::getGamificationStatus(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec("SELECT start_date, total_days FROM Gamification WHERE id=1")) {
        qWarning() << "getGamificationStatus error:" << q.lastError().text();
        return false;
    }
    if (!q.next()) {
        // Таблица пустая — создаём запись
        QSqlQuery ins(db);
        ins.exec("INSERT OR IGNORE INTO Gamification (id, start_date, total_days) VALUES (1, date('now'), 90)");
        q.exec("SELECT start_date, total_days FROM Gamification WHERE id=1");
        if (!q.next()) return false;
    }
    QString startDate = q.value(0).toString();
    int totalDays     = q.value(1).toInt();

    // Текущий день = разница дат + 1
    QSqlQuery dayQ(db);
    dayQ.prepare("SELECT CAST(julianday('now') - julianday(:sd) AS INTEGER) + 1");
    dayQ.bindValue(":sd", startDate);
    dayQ.exec();
    int currentDay = 1;
    if (dayQ.next()) currentDay = qMax(1, qMin(dayQ.value(0).toInt(), totalDays));

    QJsonObject obj;
    obj["start_date"]   = startDate;
    obj["total_days"]   = totalDays;
    obj["current_day"]  = currentDay;
    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qDebug() << "getGamificationStatus OK: day=" << currentDay << "/" << totalDays;
    return true;
}

bool UserManager::setGamificationDay(int day)
{
    if (!db.isOpen()) return false;
    if (day < 1) day = 1;
    // start_date = today - (day-1) days
    QSqlQuery q(db);
    q.prepare("UPDATE Gamification SET start_date = date('now', :offset) WHERE id=1");
    q.bindValue(":offset", QString("-%1 days").arg(day - 1));
    if (!q.exec()) {
        qWarning() << "setGamificationDay error:" << q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0) {
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO Gamification (id, start_date, total_days) VALUES (1, date('now', :offset), 90)");
        ins.bindValue(":offset", QString("-%1 days").arg(day - 1));
        ins.exec();
    }
    qDebug() << "setGamificationDay OK: day=" << day;
    return true;
}

bool UserManager::resetGamification()
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec("UPDATE Gamification SET start_date = date('now') WHERE id=1")) {
        qWarning() << "resetGamification error:" << q.lastError().text();
        return false;
    }
    qDebug() << "resetGamification OK";
    return true;
}

// ==============================================================================
// АДМИН: УПРАВЛЕНИЕ НАГРАДАМИ ЗА ДОСТИЖЕНИЯ
// ==============================================================================

bool UserManager::getAllAchievements(QString &jsonData)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec("SELECT id, key, name, description, category, reward, threshold FROM Achievements ORDER BY id")) {
        qWarning() << "getAllAchievements error:" << q.lastError().text();
        return false;
    }
    QJsonArray arr;
    while (q.next()) {
        QJsonObject obj;
        obj["id"]          = q.value(0).toInt();
        obj["key"]         = q.value(1).toString();
        obj["name"]        = q.value(2).toString();
        obj["description"] = q.value(3).toString();
        obj["category"]    = q.value(4).toString();
        obj["reward"]      = q.value(5).toInt();
        obj["threshold"]   = q.value(6).toInt();
        arr.append(obj);
    }
    jsonData = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    qDebug() << "getAllAchievements OK:" << arr.size() << "entries";
    return true;
}

bool UserManager::updateAchievementReward(int achievementId, int newReward)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE Achievements SET reward = :reward WHERE id = :id");
    q.bindValue(":reward", newReward);
    q.bindValue(":id", achievementId);
    if (!q.exec()) {
        qWarning() << "updateAchievementReward error:" << q.lastError().text();
        return false;
    }
    qDebug() << "updateAchievementReward OK: id=" << achievementId << "reward=" << newReward;
    return true;
}

// ==============================================================================
// VIP ПЕРСОНАЛЬНЫЕ ПРОГРАММЫ
// ==============================================================================

bool UserManager::setVipSubscription(int user_id, int status)
{
    if (!db.isOpen()) {
        qWarning() << "DB not open in setVipSubscription";
        return false;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE USERS SET VipSubscription = :status WHERE user_id = :uid");
    q.bindValue(":status", status);
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "setVipSubscription error:" << q.lastError().text();
        return false;
    }

    // If disabling VIP — clear program and weights
    if (status == 0) {
        deleteVipProgram(user_id);
        clearVipExerciseWeights(user_id);
        resetWorkoutProgress(user_id);
        clearWorkoutStats(user_id);
    }

    qDebug() << "setVipSubscription OK: user" << user_id << "status=" << status;
    return true;
}

bool UserManager::getVipSubscription(int user_id, int &status)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT COALESCE(VipSubscription, 0) FROM USERS WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    if (!q.exec() || !q.next()) {
        status = 0;
        return false;
    }
    status = q.value(0).toInt();
    return true;
}

bool UserManager::createVipProgram(int user_id, const QString &programName,
                                   const QString &workout1Json, const QString &workout2Json,
                                   const QString &workout3Json)
{
    if (!db.isOpen()) return false;

    QString now = QDateTime::currentDateTime().toString(Qt::ISODate);

    // Delete existing if any
    QSqlQuery del(db);
    del.prepare("DELETE FROM Vip_Programs WHERE user_id = :uid");
    del.bindValue(":uid", user_id);
    del.exec();

    // Insert new
    QSqlQuery q(db);
    q.prepare("INSERT INTO Vip_Programs (user_id, program_name, workout1_json, workout2_json, workout3_json, created_at) "
              "VALUES (:uid, :name, :w1, :w2, :w3, :date)");
    q.bindValue(":uid", user_id);
    q.bindValue(":name", programName);
    q.bindValue(":w1", workout1Json);
    q.bindValue(":w2", workout2Json);
    q.bindValue(":w3", workout3Json);
    q.bindValue(":date", now);

    if (!q.exec()) {
        qWarning() << "createVipProgram error:" << q.lastError().text();
        return false;
    }

    // Reset workout progress for fresh start
    resetWorkoutProgress(user_id);
    clearVipExerciseWeights(user_id);
    clearWorkoutStats(user_id);

    qDebug() << "createVipProgram OK: user" << user_id << "program:" << programName;
    return true;
}

bool UserManager::loadVipProgram(int user_id, QString &jsonData)
{
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare("SELECT program_name, workout1_json, workout2_json, workout3_json, created_at "
              "FROM Vip_Programs WHERE user_id = :uid");
    q.bindValue(":uid", user_id);

    if (!q.exec()) {
        qWarning() << "loadVipProgram error:" << q.lastError().text();
        return false;
    }

    if (!q.next()) {
        jsonData = "{}";
        return false;
    }

    QJsonObject obj;
    obj["program_name"] = q.value(0).toString();
    obj["workout1"] = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).array();
    obj["workout2"] = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).array();
    obj["workout3"] = QJsonDocument::fromJson(q.value(3).toString().toUtf8()).array();
    obj["created_at"] = q.value(4).toString();

    jsonData = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qDebug() << "loadVipProgram OK: user" << user_id;
    return true;
}

bool UserManager::deleteVipProgram(int user_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Vip_Programs WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    bool ok = q.exec();
    qDebug() << "deleteVipProgram:" << (ok ? "OK" : "FAILED") << "user=" << user_id;
    return ok;
}

bool UserManager::clearVipExerciseWeights(int user_id)
{
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM Vip_Exercise_Weights WHERE user_id = :uid");
    q.bindValue(":uid", user_id);
    bool ok = q.exec();
    qDebug() << "clearVipExerciseWeights:" << (ok ? "OK" : "FAILED") << "user=" << user_id;
    return ok;
}

bool UserManager::saveVipExerciseWeight(int user_id, const QString &exerciseName, float a1, float a2, float a3)
{
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare("SELECT id FROM Vip_Exercise_Weights WHERE user_id=:uid AND exercise_name=:name");
    q.bindValue(":uid", user_id);
    q.bindValue(":name", exerciseName);
    q.exec();

    if (q.next()) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE Vip_Exercise_Weights SET approach1=:a1, approach2=:a2, approach3=:a3 "
                    "WHERE user_id=:uid AND exercise_name=:name");
        upd.bindValue(":a1", a1);
        upd.bindValue(":a2", a2);
        upd.bindValue(":a3", a3);
        upd.bindValue(":uid", user_id);
        upd.bindValue(":name", exerciseName);
        return upd.exec();
    }

    QSqlQuery ins(db);
    ins.prepare("INSERT INTO Vip_Exercise_Weights (user_id, exercise_name, approach1, approach2, approach3) "
                "VALUES (:uid, :name, :a1, :a2, :a3)");
    ins.bindValue(":uid", user_id);
    ins.bindValue(":name", exerciseName);
    ins.bindValue(":a1", a1);
    ins.bindValue(":a2", a2);
    ins.bindValue(":a3", a3);
    return ins.exec();
}

bool UserManager::loadVipExerciseWeight(int user_id, const QString &exerciseName, float &a1, float &a2, float &a3)
{
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare("SELECT approach1, approach2, approach3 FROM Vip_Exercise_Weights "
              "WHERE user_id=:uid AND exercise_name=:name");
    q.bindValue(":uid", user_id);
    q.bindValue(":name", exerciseName);

    if (q.exec() && q.next()) {
        a1 = q.value(0).toFloat();
        a2 = q.value(1).toFloat();
        a3 = q.value(2).toFloat();
        return true;
    }
    a1 = a2 = a3 = 0;
    return false;
}
