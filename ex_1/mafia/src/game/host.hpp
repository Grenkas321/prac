// Ведущий: единственный, кто меняет GameState. Готовит партию, ведёт фазы,
// собирает запросы, проверяет ответы, объявляет результаты.
//
// Подготовка партии, день (обсуждение, голосование), ночь (сбор ходов и
// разбор — game/night_rules.hpp), конец игры (game/victory.hpp), итог.
//
// Ведущий однопоточен: игроки отвечают в своих нитях, но ведущий видит
// только Engine::exchange — «разослал, дождался». Поэтому GameState живёт
// без мьютекса (см. этап 7).
#pragma once

#include "core/random.hpp"
#include "core/shared_ptr.hpp"
#include "engine/engine.hpp"
#include "log/game_log.hpp"
#include "game/protocol.hpp"
#include "game/state.hpp"
#include "game/victory.hpp"
#include "game/night_rules.hpp"
#include "game/voting.hpp"
#include "roles/player.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace mafia {

struct HostConfig {
    int players = 8;                    // N > 4
    int mafia_divisor = 3;              // k >= 3
    std::uint64_t seed = 1;             // одно зерно — одна партия
    bool execute_on_first_day = false;  // первый день — без голосования
    // Журнал партии в файлах (краткий или полный). Пусто — без файлов.
    // Тайное пишется только туда; в консоль — никогда.
    SharedPtr<GameLog> log;
    // Предел партии: после этого дня — ничья. Защита от бесконечной партии,
    // а не правило игры: при 100 игроках партия длится до ~50 дней, запас — с лихвой.
    int max_days = 317;
    // Тихий режим (большие партии): протокол — только в журнал, в консоль —
    // точка на каждый день и короткий итог. Без журнала протокол не пишется никуда.
    bool quiet = false;
    // Открытые объявления: роль выбывшего, кто убил ночью, кого спасли.
    // Закрытые (по умолчанию): только «мафия / не мафия», остальное — тайна.
    bool open_announcements = false;
    // Место пользователя (--interactive). Ведущий только объявляет его и
    // сообщает пользователю роль; ходы за этим местом спрашивает в консоли
    // движок-обёртка (engine/console_engine.hpp). ИИ это место не отдаётся.
    std::optional<PlayerId> human_seat;
    // Дополнительные роли (из конфига). Мафиозные замещают обычных мафиози
    // (хотя бы один обычный остаётся), остальные — мирных жителей.
    std::vector<RoleKind> extra_roles;

    // Как создать игрока по роли. Пусто — PlayerFactory::create.
    // Нужна тестам (игроки с заданными голосами) и интерактивному режиму.
    std::function<SharedPtr<Player>(RoleKind, PlayerSetup)> make_player;

    // ИИ-игроки (этап 16): сколько мест отдать советнику и как его создать.
    // Места выбираются по зерну отдельным потоком случайных чисел — раздача
    // ролей и ходы остальных от этого не меняются. Аргументы фабрики: номер
    // игрока и порядковый номер ИИ-места (для характера).
    int ai_players = 0;
    std::function<SharedPtr<Advisor>(PlayerId, int)> make_advisor;
};

class Host {
public:
    Host(HostConfig config, SharedPtr<Engine> engine, std::ostream& out);
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // floor(N / k), но не меньше 1. Бросает invalid_argument при N <= 4 или k < 3.
    static int mafia_count(int players, int divisor);

    // Раздать роли, рассадить игроков, запустить движок, объявить начало.
    void setup();

    // Один игровой день: утро (смерти прошлой ночи), обсуждение по кругу,
    // затем голосование (если положено).
    void run_day();

    // Ночь текущего дня: все ночные ходы собираются одним пакетом,
    // затем ночь разрешается целиком. Смерти объявляются утром (run_day).
    void run_night();

    // Вся партия: день, ночь, день, ... пока не наступит конец игры или
    // предел дней; затем печатает итог. Перед вызовом — setup().
    void play();

    // Партия окончена (победа, ничья или предел дней).
    bool finished() const noexcept { return state_->finished; }

    int alive_count() const;

    // Итог: победитель, число дней, кто кем был и как выбыл,
    // и одна машиночитаемая строка «RESULT ...».
    void print_summary();

    // Сообщить игрокам об окончании (Stop) и остановить движок.
    void shutdown();

    const GameState& state() const noexcept { return *state_; }

    // Личная сводка игрока: своя роль и достоверно известное о других —
    // союзники для мафии, результаты своих проверок для комиссара.
    Briefing briefing_for(PlayerId id) const;

    // Смерти прошлой ночи, ещё не объявленные (до утра).
    const std::vector<PlayerId>& pending_deaths() const noexcept { return pending_deaths_; }

private:
    void discussion();
    void voting();
    // Один тур: голосуют все живые; candidates пуст — выбирают из всех живых.
    VoteTally collect_votes(int round, const std::vector<PlayerId>& candidates);
    void print_tally(int round, const VoteTally& tally);
    // Опрос «выгнать всех разом?» для равных лидеров.
    void kick_all_poll(const std::vector<PlayerId>& leaders, int votes_each);
    void execute(PlayerId victim);
    // Что ведущий объявляет о выбывшем: закрыто — только «мафия / не мафия»,
    // открыто — ещё и роль. И та же мысль словами для протокола.
    Disclosure disclose(const PlayerState& p) const;
    static std::string disclosure_text(const Disclosure& d);

    void morning();
    // Последствия выбывания: месть Каттани и Блейда, освобождение заключённых,
    // статус некроманта, превращения ролей. Вызывается после каждой порции смертей.
    void after_deaths(const std::vector<PlayerId>& died);
    // Выбыть сейчас (не утром): месть Каттани и Блейда.
    void kill_now(PlayerId victim, const std::string& announcement);
    void transform_roles(const std::vector<PlayerId>& died);
    // Некромант: раскрыться и воскресить (днём, раз за игру).
    void necromancer_turn();
    // Палач при ничьей; true — казнил.
    bool executioner_turn(const std::vector<PlayerId>& leaders);
    // Ночь: совет мафии и ночные способности — ходы до разбора.
    std::vector<NightOrder> mafia_council();
    std::vector<NightOrder> abilities();
    bool extra_valid(const Request& request, const Response& response) const;
    std::optional<PlayerId> find_role(RoleKind role) const;  // первый живой с этой ролью
    bool jailed(PlayerId id) const;
    bool frozen(PlayerId id) const;
    void tell(PlayerId to, const KnownIdentity& fact);
    void tell(PlayerId to, const std::string& note);

    // Проверить конец игры по составу живых; если конец — объявить и вернуть true.
    // Вызывается после каждого события, в котором кто-то выбывает.
    bool check_end();
    void finish(Verdict verdict, std::string reason, std::string code);

    // Кого роль может выбрать ночью (уже с правилами роли, например доктора).
    std::vector<PlayerId> night_targets(PlayerId actor, NightActionKind action) const;
    // Старший живой мафиози; пусто, если мафии не осталось.
    std::optional<PlayerId> boss() const;
    void secret(const std::string& line);  // только в полный журнал, никогда в консоль

    // Живые игроки в порядке круга, начиная со стартового говорящего дня.
    std::vector<PlayerId> speaking_order() const;
    PlayerId next_alive_after(PlayerId seat) const;
    bool is_alive(PlayerId id) const;
    auto seats_from(PlayerId from) const;  // ленивое представление: места по кругу от from

    // Таблица «кто кем был и как выбыл» и статистика по игрокам (для summary.txt).
    std::string summary_table() const;
    std::string player_statistics() const;
    std::string result_line() const;
    std::vector<PlayerId> alive_except(PlayerId self) const;

    SharedPtr<const PublicSnapshot> snapshot() const;
    Request make_request(RequestKind kind, PlayerId to);

    // Спросить одного игрока; недопустимый ответ — переспросить.
    // Пусто, если допустимого ответа так и не пришло.
    std::optional<Response> ask(Request request);

    // Утверждение из речи — в историю, только если оно о существующем игроке.
    // Недопустимое утверждение отбрасывается, сама речь остаётся.
    std::optional<Claim> checked_claim(const std::optional<Claim>& claim) const;

    const std::string& name(PlayerId id) const;
    std::string names(const std::vector<PlayerId>& ids) const;  // «Игрок 2, Игрок 5»
    void record(Event event);

    HostConfig config_;
    SharedPtr<Engine> engine_;
    std::ostream& console_;
    SharedPtr<GameLog> log_;
    TeeBuffer tee_;      // консоль + файл текущего круга
    std::ostream out_;   // всё публичное пишется сюда

    SharedPtr<GameState> state_;
    std::vector<SharedPtr<Player>> players_;  // индекс == PlayerId
    Rng rng_;                                 // поток 0: раздача ролей
    std::uint64_t next_request_id_ = 1;
    PlayerId first_speaker_ = 0;  // стартовый говорящий текущего дня
    std::vector<PlayerId> pending_deaths_;  // итог ночи — до утреннего объявления
    std::vector<PlayerId> pending_jail_;     // новые заключённые — объявить утром
    std::vector<std::pair<PlayerId, Disclosure>> pending_reveals_;  // взломы хакера — утром
    std::map<PlayerId, RoleKind> pending_killers_;  // кто убил ночью — для открытых объявлений
    std::vector<PlayerId> pending_saved_;           // на кого напали, но спасли — тоже
    std::optional<PlayerId> last_boss_;      // Босс до последней порции смертей (для подкидыша)
    std::vector<VoteEvent> decisive_votes_;  // голоса решающего тура (месть Блейда)
    std::string end_reason_;                // почему партия окончена — для итога
    std::string end_code_;                  // то же коротким кодом — для строки RESULT
    bool started_ = false;
};

}  // namespace mafia
