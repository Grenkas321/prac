// Тесты объявлений ведущего: закрытые (по умолчанию) и открытые.
//
//   закрытые — о выбывшем только «мафия / не мафия»; кто убил, кого спасли
//              и роль — тайна (в историю для игроков не попадают);
//   открытые — роль выбывшего, кем убит (роль, не номер), на кого напали,
//              но спасли.
// Плюс: ночь сообщает, кто убил; раскрытый маньяк — враг и после смерти.
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "game/night_rules.hpp"
#include "roles/beliefs.hpp"
#include "test_framework.hpp"

#include <sstream>
#include <string>

using namespace mafia;

namespace {

struct Game {
    std::string out;
    GameState state;
};

Game play(std::uint64_t seed, bool open, int players = 9) {
    HostConfig c;
    c.players = players;
    c.seed = seed;
    c.open_announcements = open;
    std::ostringstream out;
    Host host(c, make_shared_ptr<ThreadEngine>(), out);
    host.setup();
    host.play();
    host.shutdown();
    return Game{out.str(), host.state()};
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

void closed_keeps_secrets() {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const Game g = play(seed, false);
        CHECK(has(g.out, "Объявления ведущего: закрытые"));
        CHECK(!has(g.out, "Это был:") && !has(g.out, "Убит ") && !has(g.out, "спасли"));
        bool secret_leaked = false;
        for (const HistoryEntry& h : g.state.history) {
            if (const auto* d = std::get_if<DeathEvent>(&h.event)) {
                secret_leaked = secret_leaked || d->disclosed.role || d->killed_by;
            }
            secret_leaked = secret_leaked || std::holds_alternative<SavedEvent>(h.event);
        }
        CHECK(!secret_leaked);
    }
}

void open_reveals_roles_killers_and_saves() {
    int deaths = 0;
    int night_with_killer = 0;
    int saves = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        const Game g = play(seed, true);
        CHECK(g.state.finished);
        CHECK(has(g.out, "Объявления ведущего: открытые"));
        for (const HistoryEntry& h : g.state.history) {
            if (const auto* d = std::get_if<DeathEvent>(&h.event)) {
                ++deaths;
                const RoleKind real = g.state.players[static_cast<std::size_t>(d->victim)].role;
                CHECK(d->disclosed.role == real);  // объявлена настоящая роль
                CHECK(d->disclosed.is_mafia == (team_of(real) == Team::Mafia));
                if (d->cause == DeathCause::Night && d->killed_by) {
                    ++night_with_killer;
                    // Свои не убивают своих: мафия не убивает мафию, маньяк — себя.
                    CHECK(!(team_of(*d->killed_by) == Team::Mafia && team_of(real) == Team::Mafia));
                    CHECK(*d->killed_by != real || real != RoleKind::Maniac);
                }
            }
            if (const auto* s = std::get_if<SavedEvent>(&h.event)) {
                ++saves;
                CHECK(has(g.out, "на " + g.state.players[static_cast<std::size_t>(s->target)].name +
                                     " напали, но его спасли"));
            }
        }
    }
    CHECK(deaths > 200);
    CHECK(night_with_killer > 50);
    CHECK(saves > 0);  // доктор хоть раз спас за 60 партий
}

void night_report_names_killers() {
    GameState st;
    st.day = 2;
    const RoleKind roles[] = {RoleKind::Mafioso, RoleKind::Maniac, RoleKind::Civilian, RoleKind::Civilian,
                              RoleKind::Doctor, RoleKind::Commissar};
    for (PlayerId i = 0; i < 6; ++i) {
        PlayerState p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        p.role = roles[i];
        st.players.push_back(p);
    }
    auto order = [](PlayerId actor, RoleKind role, NightActionKind kind, PlayerId target, bool council = false) {
        NightOrder o;
        o.actor = actor;
        o.role = role;
        o.action.kind = kind;
        o.action.target = target;
        o.council = council;
        return o;
    };
    const NightReport r = resolve_full_night(
        st, {order(0, RoleKind::Mafioso, NightActionKind::Kill, 2, true), order(1, RoleKind::Maniac, NightActionKind::Kill, 3),
             order(5, RoleKind::Commissar, NightActionKind::Shoot, 0), order(4, RoleKind::Doctor, NightActionKind::Heal, 3)});
    std::map<PlayerId, RoleKind> killers(r.killed_by.begin(), r.killed_by.end());
    CHECK(killers.size() == 2);
    CHECK(killers.at(2) == RoleKind::Mafioso);    // убит мафией
    CHECK(killers.at(0) == RoleKind::Commissar);  // застрелен комиссаром
    CHECK(!killers.contains(3));                  // доктор спас — убийцы нет
    CHECK(std::ranges::find(r.saved, 3) != r.saved.end());
}

void revealed_maniac_counts_as_enemy() {
    // Игрок 2 голосовал против Игрока 3; Игрок 3 выбыл и объявлен маньяком.
    PublicSnapshot w;
    w.day = 3;
    for (PlayerId i = 0; i < 5; ++i) {
        PublicPlayer p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        w.players.push_back(p);
    }
    w.players[2].alive = false;
    w.players[2].disclosed = Disclosure{false, RoleKind::Maniac};
    w.history.push_back({2, Phase::Voting, VoteEvent{1, PlayerId{2}, 1}});
    const Beliefs b = Beliefs::evaluate(w, 0, RoleKind::Civilian, {});
    CHECK(b.certain_mafia(2) == true);  // враг мирных, хоть и не мафия
    CHECK(b.trust(1) > 0);              // голос против маньяка — «попал»
    CHECK(b.suspicion(1) <= 0);
}

int main() {
    test::run("закрытые: роль, убийца и спасения — тайна", &closed_keeps_secrets);
    test::run("открытые: роль, кем убит, кого спасли (60 партий)", &open_reveals_roles_killers_and_saves);
    test::run("ночь сообщает убийц", &night_report_names_killers);
    test::run("раскрытый маньяк — враг и после смерти", &revealed_maniac_counts_as_enemy);
    return test::finish();
}
