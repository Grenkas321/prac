// Тесты модели данных: принадлежность ролей командам, названия,
// и то, что из модели можно собрать состояние партии и запрос/ответ.
#include "game/protocol.hpp"
#include "game/state.hpp"
#include "game/types.hpp"
#include "test_framework.hpp"

#include <set>
#include <string_view>
#include <type_traits>
#include <variant>

using namespace mafia;

// team_of — constexpr: принадлежность проверяется ещё при компиляции.
static_assert(team_of(RoleKind::Civilian) == Team::Civilians);
static_assert(team_of(RoleKind::Commissar) == Team::Civilians);
static_assert(team_of(RoleKind::Doctor) == Team::Civilians);
static_assert(team_of(RoleKind::Mafioso) == Team::Mafia);
static_assert(team_of(RoleKind::Maniac) == Team::Loner);

// Снимок и сообщения — обычные значения: их можно копировать и перемещать.
static_assert(std::is_copy_constructible_v<Request>);
static_assert(std::is_nothrow_move_constructible_v<Response>);

void names_and_descriptions() {
    std::set<std::string_view> names;
    for (RoleKind r : kAllRoles) {
        CHECK(!role_name(r).empty());
        CHECK(!role_description(r).empty());
        names.insert(role_name(r));
    }
    CHECK(names.size() == kAllRoles.size());  // названия не повторяются
    CHECK(team_name(Team::Loner) == "Одиночка");
    CHECK(phase_name(Phase::Night) == "Ночь");
}

// Структуры заполняем «по умолчанию + присваивания», а не назначенными
// инициализаторами {.id = ...}: g++ 13 предупреждает (-Wmissing-field-initializers)
// о каждом пропущенном в них поле, даже если у поля есть значение по умолчанию.

void game_state_holds_public_history() {
    GameState st;
    for (PlayerId i = 0; i < 5; ++i) {
        PlayerState p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        st.players.push_back(p);
    }
    st.players[2].role = RoleKind::Mafioso;
    st.players[2].alive = false;
    Disclosure closed;  // закрытое объявление: только «мафия / не мафия»
    closed.is_mafia = true;
    st.players[2].disclosed = closed;

    st.day = 1;
    st.phase = Phase::Voting;

    SpeechEvent speech;
    speech.speaker = 0;
    speech.text = "Игрок 3 — мафия!";
    st.history.push_back(HistoryEntry{1, Phase::Discussion, speech});

    VoteEvent vote;
    vote.voter = 0;
    vote.target = 2;
    st.history.push_back(HistoryEntry{1, Phase::Voting, vote});

    DeathEvent death;
    death.victim = 2;
    death.cause = DeathCause::Executed;
    death.disclosed = closed;
    st.history.push_back(HistoryEntry{1, Phase::Voting, death});

    CHECK(st.players.size() == 5);
    CHECK(std::holds_alternative<DeathEvent>(st.history.back().event));
    CHECK(!std::get<DeathEvent>(st.history.back().event).killed_by);  // закрытый режим
    CHECK(!st.finished && !st.winner);
}

void night_log_remembers_doctor() {
    GameState st;
    const PlayerId doctor = 1;

    NightRecord n1;  // ночь 1: доктор лечил игрока 4
    n1.night = 1;
    n1.actor = doctor;
    n1.action.kind = NightActionKind::Heal;
    n1.action.target = 4;
    st.night_log.push_back(n1);

    NightRecord n2 = n1;  // ночь 2: лечил себя
    n2.night = 2;
    n2.action.target = doctor;
    st.night_log.push_back(n2);

    // Запрет на ночь 3 выводится из журнала: кого доктор лечил в ночь 2.
    std::optional<PlayerId> forbidden;
    for (const NightRecord& r : st.night_log) {
        if (r.night == 2 && r.actor == doctor && r.action.kind == NightActionKind::Heal) {
            forbidden = r.action.target;
        }
    }
    CHECK(forbidden == doctor);  // себя — нельзя, игрока 4 — снова можно
}

void request_shares_one_snapshot() {
    auto snapshot = make_shared_ptr<PublicSnapshot>();
    snapshot->day = 2;
    snapshot->phase = Phase::Night;
    PublicPlayer first;
    first.id = 0;
    first.name = "Игрок 1";
    snapshot->players.push_back(first);

    // Ведущий отдаёт снимок дальше как неизменяемый.
    SharedPtr<const PublicSnapshot> world = std::move(snapshot);

    Request to_doctor;
    to_doctor.id = 1;
    to_doctor.kind = RequestKind::NightMove;
    to_doctor.world = world;
    to_doctor.you.self = 0;
    to_doctor.you.role = RoleKind::Doctor;
    to_doctor.targets = {0, 1, 3};
    to_doctor.actions = {NightActionKind::Heal};

    Request to_maniac = to_doctor;  // копия делит тот же снимок
    to_maniac.id = 2;
    to_maniac.you.self = 4;
    to_maniac.you.role = RoleKind::Maniac;
    to_maniac.actions = {NightActionKind::Kill};

    CHECK(world.use_count() == 3);  // один снимок на всех адресатов
    CHECK(to_doctor.world.get() == to_maniac.world.get());
    CHECK(to_doctor.world->day == 2);

    Response r;
    r.request_id = to_doctor.id;
    r.from = 0;
    r.night.kind = NightActionKind::Heal;
    r.night.target = 3;
    CHECK(r.night.target == 3 && !r.target && r.speech.empty() && !r.choice);
}

int main() {
    std::cout << "Модель данных\n";
    test::run("названия и описания ролей", &names_and_descriptions);
    test::run("состояние партии и публичная история", &game_state_holds_public_history);
    test::run("тайный журнал ночей: вчерашняя цель доктора", &night_log_remembers_doctor);
    test::run("запросы делят один снимок", &request_shares_one_snapshot);
    return test::finish();
}
