// Player — базовый класс игрока. Общее для всех ролей:
//   * кто я: номер, имя, роль, свой поток случайных чисел;
//   * обработка запроса ведущего: handle() — единственная точка входа;
//   * выбор цели: лучшие по оценке кандидаты, среди равных — случайно;
//   * формирование ответа: request_id и from проставляются здесь,
//     наследник забыть их не может;
//   * общее дневное поведение: голос, речь, последнее слово, выбор варианта.
//
// Наследники добавляют только особенности роли:
//   * ночной ход act() и приём тайных сведений learn() — обычные методы,
//     фабрика (player_factory.hpp) подключает их через концепты;
//   * оценку целей (target_score), ответ на опрос (choose_option) и особые
//     реплики (statement, farewell) — виртуальные методы базы.
//
// Решения опираются на оценки Beliefs (roles/beliefs.hpp): база пересчитывает
// их из снимка перед каждым ответом, наследники только читают.
//
// Про нити здесь нет ничего: handle() — обычный вызов «запрос -> ответ».
// Какая нить и когда его вызывает, решает движок. Объект игрока меняет
// только та нить, что вызывает handle(), — других писателей у него нет.
#pragma once

#include "core/random.hpp"
#include "game/protocol.hpp"
#include "game/types.hpp"
#include "core/shared_ptr.hpp"
#include "roles/advisor.hpp"
#include "roles/beliefs.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mafia {

// Всё, что нужно для создания игрока любой роли.
struct PlayerSetup {
    PlayerSetup(PlayerId id_, std::string name_, Rng rng_) : id(id_), name(std::move(name_)), rng(rng_) {}

    PlayerId id;
    std::string name;
    Rng rng;  // личный поток: Rng::stream(seed, id + 1)
};

class Player {
public:
    // Игрок не копируется и не перемещается: зарегистрированные фабрикой
    // обработчики хранят указатель на сам объект.
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    virtual ~Player() = default;

    PlayerId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    RoleKind role() const noexcept { return role_; }
    Team team() const noexcept { return team_of(role_); }

    bool has_night_move() const noexcept { return static_cast<bool>(night_move_); }
    bool receives_secrets() const noexcept { return static_cast<bool>(secret_sink_); }

    // Единственная точка входа: принять запрос ведущего, вернуть ответ.
    // Перед ответом пересчитывает оценки по снимку (см. beliefs()).
    Response handle(const Request& request);

    // Внешний советник (ИИ-агент). Подключается до запуска движка; пусто — нет.
    void set_advisor(SharedPtr<Advisor> advisor) { advisor_ = std::move(advisor); }
    bool has_advisor() const noexcept { return static_cast<bool>(advisor_); }

    // Оценки игрока по состоянию на последний запрос. Только чтение.
    const Beliefs& beliefs() const noexcept { return beliefs_; }

    // Общее поведение всех ролей (его же требует концепт RoleClass).
    Response vote(const Request& request);
    Response speak(const Request& request);
    Response last_word(const Request& request);
    Response choose(const Request& request);

protected:
    Player(PlayerSetup setup, RoleKind role);

    enum class Purpose { Vote, Night };

    // Выбор цели: максимальная оценка target_score, среди равных — случайно.
    // Пусто, если выбирать не из чего.
    std::optional<PlayerId> choose_target(const Request& request, Purpose purpose);
    std::optional<PlayerId> choose_target(const Request& request, Purpose purpose,
                                          const std::vector<PlayerId>& candidates);

    // Насколько роль хочет выбрать кандидата. База (мирный взгляд): днём —
    // голос против самого подозрительного, ночью предпочтений нет.
    virtual int target_score(const Request& request, Purpose purpose, PlayerId candidate) const;

    // Какой вариант выбрать в запросе Choice («выгнать всех?»: 0 — да, 1 — нет;
    // request.targets — о ком вопрос). База: против, если в списке я сам;
    // за, если в среднем список подозрителен. options гарантированно не пуст.
    virtual std::size_t choose_option(const Request& request);

    // Что сказать: текст и утверждение («X — мафия» / «X — мирный»).
    // Реплика не должна выдавать роль говорящего.
    struct Statement {
        std::string text;
        std::optional<Claim> claim;
    };
    virtual Statement statement(const Request& request);
    virtual Statement farewell(const Request& request);

    // Помощники для реплик.
    std::vector<PlayerId> alive_others(const Request& request) const;
    std::string name_of(const Request& request, PlayerId id) const;
    Statement accuse(const Request& request, PlayerId suspect);
    Statement defend(const Request& request, PlayerId honest);
    Statement neutral();

    // Оценка -> целое для target_score (сотые доли).
    static int score(double value);

    // Общая политика ночного хода: действие и цель по виду действия и оценкам.
    Response act_by_policy(const Request& request);
    // Кандидат с наибольшей оценкой; среди равных — случайно.
    std::optional<PlayerId> best_target(const std::vector<PlayerId>& candidates,
                                        const std::function<double(PlayerId)>& value);

    // Как видит стол сторонний наблюдатель (только публичное знание).
    // Нужно мафии: подставлять выгодно того, кого город и так подозревает.
    const Beliefs& town() const noexcept { return town_; }

    Rng& rng() noexcept { return rng_; }

private:
    friend class PlayerFactory;

    using NightMove = std::function<Response(const Request&)>;
    using SecretSink = std::function<void(const KnownIdentity&)>;

    // Спросить советника; любое исключение — «совета нет».
    std::optional<AdvisorAnswer> consult(const Request& request) noexcept;

    // Передать наследнику тайные сведения из личной сводки запроса.
    void absorb(const Briefing& briefing);

    PlayerId id_;
    std::string name_;
    RoleKind role_;
    Rng rng_;

    // Личная память оценок — пересчитывается из снимка на каждый запрос.
    Beliefs beliefs_;
    Beliefs town_;
    SharedPtr<Advisor> advisor_;  // его нить — только эта; общие части советник защищает сам

    // Подключаются фабрикой только для ролей с этими способностями.
    NightMove night_move_;
    SecretSink secret_sink_;
};

}  // namespace mafia
