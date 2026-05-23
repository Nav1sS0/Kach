#ifndef USERMANAGER_H
#define USERMANAGER_H

#include <QString>
#include <QSqlDatabase>
#include <QSqlError>
#include <QDate>


class UserManager
{
public:
    UserManager();

    //Функция регистрации пользователся
    bool registerUser(const QString &firstName,
                      const QString &lastName,
                      const QString &email,
                      const QString &passwordHash,
                      const QString &gender,
                      const QString &birthDate,
                      double height,
                      double weight,
                      QString &errorMessage,
                      int& userId);

    //Проверка учетных данных
    bool authenticateUser(const QString &email, const QString &password,int& userId);

    //Получение текущего состояния подписки
    int checkSubscriptionStatus(const QString &userId);

    //Загрузка данных из бд
    bool loadUserData(int user_id, QString &first_name, QString &last_name, QString &gender, QString &Age, double &height, double &weight, QString &Goal, int &PlanTrainninnUserData, int &standardSubscription, int &vipSubscription, int &subscriptionFrozen, int &feedbackSubscription);

    //Обновление данных в БД
    bool updateUserData(int user_id, QString &first_name, QString &last_name,QString &gender, QString &Age, double &height, double &weight, QString &Goal);

    bool saveImage(int user_id, QByteArray &image);

    bool getImage(int user_id, QByteArray &imageData);

    bool saveEPlanTrainningUserData(int user_id, int &EPlanTrainningUser);

    //Замеры
    bool saveUserMeasurements(int &user_id, QString &Data, QString &Weight, QString &Chest_Circumference, QString &Waist_Circumference, QString &Hip_Circumference, QString &Total_Steps_Day);

    bool createMeasurement(int user_id, const QString &date, const QString &weight,
                           const QString &chest, const QString &waist, const QString &hips,
                           const QString &steps, int &measurement_id);

    bool saveMeasurementPhoto(int user_id, int measurement_id, const QString &photoType, const QByteArray &photoData);

    bool getMeasurementPhoto(int measurement_id, const QString &photoType, QByteArray &photoData);

    bool loadUserMeasurements(int user_id, QString &jsonData);

    // ✅ ПРОГРЕСС ПО НЕДЕЛЯМ (блокировка тренировок)
    bool getWorkoutProgress(int user_id, int &current_week,
                            bool &w1_done, bool &w2_done, bool &w3_done,
                            QString &w1_date, QString &w2_date, QString &w3_date,
                            bool &plan_completed);
    bool markWorkoutDone(int user_id, int workout_num, const QString &date,
                         int &kachballs_awarded);
    void tryAdvanceWeek(int user_id);

    // ── Призовые баллы (настраиваются админом) ─────────────────────────────
    // Ключи: "workout_done", "dish_added", "cooked_dish", "lesson_viewed"
    int  getBonusAmount(const QString &key, int defaultAmount = 0);
    bool setBonusAmount(const QString &key, int amount);
    bool getAllBonusSettings(QString &jsonData);
    // Награда за добавление блюда в рацион (с дедупом «уникальное блюдо в день»)
    bool addDishToDiet(int user_id, const QString &recipe_name, const QString &date,
                       int &kachballs_awarded);

    // ✅ Сброс прогресса тренировок
    bool resetWorkoutProgress(int user_id);

    // ✅ Дата начала плана (для системы смены через 2 месяца)
    bool savePlanStartDate(int user_id, const QString &datetime);
    bool getPlanStartDate(int user_id, QString &datetime);
    bool isPlanChangeDue(int user_id, bool &due);
    // Через 90 дней план сбрасывается принудительно (для Lite/Plus —
    // через resetWorkoutProgress + clear PlanTrainning, для VIP — только
    // показывается диалог «обратитесь к тренеру»).
    bool isPlanResetDue(int user_id, bool &due);

    // ==================== РЕЦЕПТЫ ====================

    // Загрузить все рецепты (возвращает JSON-массив)
    bool loadAllRecipes(QString &jsonData);

    // Загрузить детали одного рецепта (ингредиенты + шаги)
    bool loadRecipeDetails(int recipe_id, QString &jsonData);

    // Добавить новый рецепт (admin)
    bool addRecipe(const QString &name,
                   const QString &mealType,
                   int kcal, int protein, int fat, int carbs,
                   double rating,
                   const QString &ingredient,
                   const QString &img,
                   const QString &ingredientsJson,
                   const QString &stepsJson,
                   int &newId);

    // ==================== ОТЗЫВЫ ====================

    // Сохранить / обновить отзыв пользователя
    bool saveReview(int recipe_id, int user_id, const QString &user_name,
                    int rating, const QString &comment);

    // Загрузить все отзывы рецепта → JSON-массив
    bool loadReviews(int recipe_id, QString &jsonData);

    // Сохранить/обновить рацион на конкретную дату
    bool saveDailyNutrition(int user_id, const QString &date,
                            const QString &mealsJson,
                            int kcal, int protein, int fat, int carbs);

    // Загрузить рацион на конкретную дату
    bool loadDailyNutrition(int user_id, const QString &date,
                            QString &mealsJson,
                            int &kcal, int &protein, int &fat, int &carbs);

    // Загрузить историю всех дней с данными
    bool loadNutritionHistory(int user_id, QString &jsonData);

    // ==================== АДМИН-ПАНЕЛЬ ====================
    bool getAllUsers(QString &jsonData);
    bool setSubscription(int user_id, int status);
    bool deleteRecipe(int recipe_id);
    bool deleteReview(int review_id, int recipe_id);
    bool getUserWorkoutInfo(int user_id, QString &jsonData);
    bool adminResetUserPlan(int user_id);

    // ✅ Заморозка подписки (1 = заморожена, 0 = активна)
    bool setSubscriptionFrozen(int user_id, int frozen);
    bool getSubscriptionFrozen(int user_id, int &frozen);
    // ✅ Заморозка с авто-разморозкой через N дней + лимит 2 раза в год
    bool freezeSubscription(int user_id, int days, QString &errorMessage);
    bool unfreezeSubscription(int user_id);
    // Возвращает список user_id, у которых freeze_ends_at истёк — для авто-разморозки
    bool getExpiredFreezes(QList<int> &userIds);

    // ✅ Третий тариф — "С обратной связью"
    bool setFeedbackSubscription(int user_id, int status);
    bool getFeedbackSubscription(int user_id, int &status);

    // ==================== АВТО-БИЛЛИНГ (рекуррентные платежи) ==============
    // Структура одной записи "к списанию"
    struct BillingDueRow {
        int     userId;
        QString tier;              // 'lite' | 'feedback'
        QString paymentMethodId;
        int     retryCount;        // 0..3
        QString email;             // для уведомлений
        QString firstName;         // для персонализации письма
    };

    // Сохранить payment_method (после первой успешной оплаты на сайте)
    bool billingSaveMethod(int user_id,
                           const QString &paymentMethodId,
                           const QString &paymentMethodTitle,
                           const QString &tier,
                           int periodDays = 30);

    // Список пользователей, у которых next_billing_date <= now AND auto_renew=1
    // AND payment_method_id != '' AND tier IN ('lite','feedback') AND NOT VIP.
    // Используется сайтом-cron'ом.
    bool billingGetDue(QList<BillingDueRow> &rows);

    // Успешное списание: продлить срок (+periodDays), сбросить retry,
    // разморозить (если была билинг-заморозка) и обновить last_billing_*.
    bool billingRecordSuccess(int user_id, int periodDays = 30);

    // Неуспешное списание: увеличить retry_count.
    // Если retry_count < 3 → отложить next_billing_date по схеме (+1д / +3д / +5д).
    // Если retry_count == 3 → выставить subscription_frozen=1, freeze_reason='billing'.
    // outFrozen=true означает, что пользователь только что заморожен.
    bool billingRecordFailure(int user_id, bool &outFrozen);

    // Отменить авто-продление (пользователь через ЛК). Карта остаётся, но списываться
    // больше не будет до повторного оформления. Текущая подписка работает до конца периода.
    bool billingCancelAutoRenew(int user_id);

    // Состояние биллинга — для страницы /account на сайте (JSON).
    bool billingGetState(int user_id, QString &jsonData);

    // ✅ Чат: история сообщений и отправка
    bool sendChatMessage(int user_id, bool fromAdmin, const QString &text, qint64 &messageId);
    bool loadChatHistory(int user_id, QString &jsonData);
    // Сохранить фото-сообщение (text может быть пустой). Возвращает messageId.
    bool sendChatPhoto(int user_id, bool fromAdmin, const QByteArray &photoData, qint64 &messageId);
    // Загрузить бинарь фото из конкретного сообщения чата.
    bool getChatPhoto(qint64 message_id, QByteArray &photoData);
    // Стереть всю переписку с пользователем (вызывается из «Очистить чат»
    // как админом, так и пользователем). Возвращает количество удалённых строк.
    bool clearChatHistory(int user_id, int &deletedCount);

    // ──────────────── Чат техподдержки (доступен всем) ────────────────
    // Аналогичен чату с тренером, но в отдельной таблице Support_Messages
    // и БЕЗ проверки подписки — обращения принимаются от любого юзера.
    bool sendSupportMessage(int user_id, bool fromAdmin, const QString &text, qint64 &messageId);
    bool sendSupportPhoto(int user_id, bool fromAdmin, const QByteArray &photoData, qint64 &messageId);
    bool getSupportPhoto(qint64 message_id, QByteArray &photoData);
    bool loadSupportHistory(int user_id, QString &jsonData);
    bool markSupportRead(int user_id, bool byAdmin);
    bool countSupportUnreadForUser(int user_id, int &count);
    bool getAdminSupportConversations(QString &jsonData);
    bool clearSupportHistory(int user_id, int &deletedCount);
    bool markChatRead(int user_id, bool byAdmin);
    bool getAdminConversations(QString &jsonData);   // список юзеров с непрочитанными
    bool countUnreadForUser(int user_id, int &count);

    // ✅ Приготовленные блюда (фото) + начисление баллов
    bool saveCookedDish(int user_id, int recipe_id, const QByteArray &photoData, int &dishId);
    bool loadCookedDishes(int user_id, QString &jsonData);
    bool getCookedDishPhoto(int dish_id, QByteArray &photoData);

    // ✅ Просмотренные уроки + начисление баллов (одноразовое)
    bool markLessonViewed(int user_id, const QString &lessonKey, int &awardedBalls);
    bool loadViewedLessons(int user_id, QString &jsonData);

    // ✅ Избранные рецепты
    bool addFavoriteRecipe(int user_id, int recipe_id);
    bool removeFavoriteRecipe(int user_id, int recipe_id);
    bool loadFavoriteRecipes(int user_id, QString &jsonData);

    // ✅ Регистрация push-устройств
    bool registerDevice(int user_id, const QString &token, const QString &platform);
    bool unregisterDevice(int user_id, const QString &token);
    bool loadUserDevices(int user_id, QString &jsonData);
    // Все устройства всех пользователей — для cron-рассылки пушей с сайта.
    // Возвращает JSON-массив [{user_id, first_name, token, platform}, ...].
    bool loadAllDevices(QString &jsonData);
    // Снять регистрацию устройства по токену (когда FCM возвращает 404/UNREGISTERED).
    bool removeDeviceByToken(const QString &token);

    // ✅ Начисление баллов (универсальный helper)
    bool addKachballs(int user_id, int amount);

    // ==================== VIP ПЕРСОНАЛЬНЫЕ ПРОГРАММЫ ====================
    bool setVipSubscription(int user_id, int status);
    bool getVipSubscription(int user_id, int &status);
    bool createVipProgram(int user_id, const QString &programName,
                          const QString &workout1Json, const QString &workout2Json,
                          const QString &workout3Json);
    bool loadVipProgram(int user_id, QString &jsonData);
    bool deleteVipProgram(int user_id);
    bool clearVipExerciseWeights(int user_id);

    // ==================== СТАТИСТИКА ТРЕНИРОВОК ====================
    bool computeAndSaveWeeklyStats(int user_id);
    bool getWorkoutStats(int user_id, QString &jsonData);
    bool clearWorkoutStats(int user_id);

    // ==================== ЗАМЕНЫ УПРАЖНЕНИЙ ====================
    // Сохранить/удалить замену (пустая substituteName = удалить)
    bool saveExerciseSubstitution(int user_id, const QString &originalName, const QString &substituteName);
    // Загрузить все замены пользователя в виде JSON {"оригинал":"замена",...}
    bool loadExerciseSubstitutions(int user_id, QString &jsonData);

    // ==================== ДРУЗЬЯ ====================
    // Получить свой friendId
    bool getUserFriendId(int user_id, int &friendId);
    // Найти пользователя по friendId (исключая себя)
    bool searchUserByFriendId(int myUserId, int targetFriendId, QString &jsonData);
    // Отправить запрос в друзья (by friendId target); toUserId — получатель (для push-уведомления)
    bool sendFriendRequest(int fromUserId, int targetFriendId, QString &errorMessage, int &toUserId);
    // Получить входящие запросы в друзья (status='pending', to_user_id=myId)
    bool getIncomingFriendRequests(int userId, QString &jsonData);
    // Принять запрос
    bool acceptFriendRequest(int myUserId, int requestId);
    // Отклонить запрос
    bool declineFriendRequest(int myUserId, int requestId);
    // Получить список друзей с именем, возрастом, целью, аватаром
    bool getFriends(int userId, QString &jsonData);
    // Удалить друга
    bool removeFriend(int myUserId, int friendUserId);


    // ==================== ДОСТИЖЕНИЯ И КАЧБАЛЛЫ ====================
    // Получить все достижения пользователя (полученные + доступные)
    bool getUserAchievements(int user_id, QString &jsonData);
    // Проверить и выдать новые достижения; возвращает JSON новых
    bool checkAndGrantAchievements(int user_id, QString &newAchievementsJson);
    // Получить число качбаллов
    bool getUserKachballs(int user_id, int &kachballs);

    bool getLeaderboard(QString &jsonData);          // топ-100 по кач-баллам
    bool resetAllKachballs();                         // обнуление баллов всех
    bool getGamificationStatus(QString &jsonData);   // текущее состояние геймификации
    bool setGamificationDay(int day);                // установить текущий день вручную
    bool resetGamification();                         // сбросить геймификацию на день 1

    // ── Админ: управление наградами за достижения ──
    bool getAllAchievements(QString &jsonData);
    bool updateAchievementReward(int achievementId, int newReward);

    // ==================== ЕЖЕНЕДЕЛЬНЫЕ МИССИИ ====================
    // Получить миссии текущей недели; если нет — создаёт запись
    bool getWeeklyMissions(int user_id, int year, int week, QString &jsonData);
    // Обновить выполнение миссий; автоматически начисляет кач-баллы
    bool updateWeeklyMissions(int user_id, int year, int week,
                              bool steps_done, bool workouts_done, bool nutrition_done,
                              int &kachballs_awarded, QString &jsonData);

    // ✅ SAVE TRAINING - Все 19 тренировок
    bool saveTrainingButtockBridgeP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingChestPressP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingHorizontalThrustP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingOneLegBenchPressP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingPressOnMatP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingVerticalThrustP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingPressOnBallP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingKettlebellSquatP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingRomanianDeadliftP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingKneePushupP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingHipAbductionP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingTricepExtensionP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingAssistedPullupP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingBulgarianSplitSquatP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingShoulderPressP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingCableFlyP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingLegExtensionP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingPlankP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool saveTrainingChestFlyP1(int user_id,float &approach1, float &approach2, float &approach3);

    // ✅ SAVE TRAINING - Plan 2 (5 новых упражнений)
    bool saveTrainingHipAbductionLeanP2(int user_id, float &approach1, float &approach2, float &approach3);
    bool saveTrainingPushupP2(int user_id, float &approach1, float &approach2, float &approach3);
    bool saveTrainingHipAbduction90P2(int user_id, float &approach1, float &approach2, float &approach3);
    bool saveTrainingPullupP2(int user_id, float &approach1, float &approach2, float &approach3);
    bool saveTrainingLegExtensionFrogP2(int user_id, float &approach1, float &approach2, float &approach3);

    // ✅ GET TRAINING - Все 19 тренировок
    bool GetSaveTrainingButtockBridgeP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingChestPressP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingHorizontalThrustP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingOneLegBenchPressP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingPressOnMatP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingVerticalThrustP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingPressOnBallP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingKettlebellSquatP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingRomanianDeadliftP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingKneePushupP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingHipAbductionP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingTricepExtensionP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingAssistedPullupP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingBulgarianSplitSquatP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingShoulderPressP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingCableFlyP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingLegExtensionP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingPlankP1(int user_id,float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingChestFlyP1(int user_id,float &approach1, float &approach2, float &approach3);

    // ✅ GET TRAINING - Plan 2 (5 новых упражнений)
    bool GetSaveTrainingHipAbductionLeanP2(int user_id, float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingPushupP2(int user_id, float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingHipAbduction90P2(int user_id, float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingPullupP2(int user_id, float &approach1, float &approach2, float &approach3);
    bool GetSaveTrainingLegExtensionFrogP2(int user_id, float &approach1, float &approach2, float &approach3);

    // ✅ SAVE TRAINING - Plan 3 (9 новых упражнений)
    bool saveTrainingCraneP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingShoulderSideRaiseSitP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingGoodmorningHackP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingShoulderRaiseOneHandBenchP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingFroggyStandP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingHackSquatP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingPulloverP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingCrossoverDiagonalKickP3(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingWallAbductionOneP3(int user_id, float &a1, float &a2, float &a3);
    // ✅ GET TRAINING - Plan 3
    bool GetSaveTrainingCraneP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingShoulderSideRaiseSitP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingGoodmorningHackP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingShoulderRaiseOneHandBenchP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingFroggyStandP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingHackSquatP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingPulloverP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingCrossoverDiagonalKickP3(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingWallAbductionOneP3(int user_id, float &a1, float &a2, float &a3);

    // ✅ SAVE TRAINING - Plan 4 (6 новых упражнений)
    bool saveTrainingCableKickbackP4(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingCrossoverStepUpP4(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingDipsP4(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingCableStorkP4(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingGluteBridgeSingleLegP4(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingCableRotationP4(int user_id, float &a1, float &a2, float &a3);
    // ✅ GET TRAINING - Plan 4
    bool GetSaveTrainingCableKickbackP4(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingCrossoverStepUpP4(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingDipsP4(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingCableStorkP4(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingGluteBridgeSingleLegP4(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingCableRotationP4(int user_id, float &a1, float &a2, float &a3);

    // ✅ SAVE TRAINING - Plan 7 (4 новых упражнения)
    bool saveTrainingNegativePullupP7(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingBarbellBicepCurlP7(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingLegRaiseElbowP7(int user_id, float &a1, float &a2, float &a3);
    bool saveTrainingBrachialiscurlP7(int user_id, float &a1, float &a2, float &a3);
    // ✅ GET TRAINING - Plan 7
    bool GetSaveTrainingNegativePullupP7(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingBarbellBicepCurlP7(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingLegRaiseElbowP7(int user_id, float &a1, float &a2, float &a3);
    bool GetSaveTrainingBrachialiscurlP7(int user_id, float &a1, float &a2, float &a3);

    // VIP-вес упражнений (generic save/load)
    bool saveVipExerciseWeight(int user_id, const QString &exerciseName, float a1, float a2, float a3);
    bool loadVipExerciseWeight(int user_id, const QString &exerciseName, float &a1, float &a2, float &a3);

private:
    //Объект базы данных
    QSqlDatabase db;

    struct User {
        QString firstName;
        QString lastName;
        QString email;
        QString passwordHash;
        QString gender;
        QDate birthDate;
        double height;
        double weight;
    };

    //Объект данных пользователя
    User userData;

    //Метод для хэширования пароля (legacy — статическая соль)
    void hashPassword(QString &password);
    // ✅ Новый метод: хеш пароля с per-user солью (защита от rainbow tables)
    QString hashPasswordWithSalt(const QString &password, const QByteArray &salt) const;
    // ✅ Генерация криптографически случайной соли (16 байт hex)
    QByteArray generateSalt() const;

    // Инициализация таблиц БД
    void initializeTables();

    // Начальные данные рецептов (вызывается один раз, если таблица пустая)
    void seedInitialRecipes();

    // Начальные данные достижений (вызывается один раз, если таблица пустая)
    void seedAchievements();
};

#endif // USERMANAGER_H
