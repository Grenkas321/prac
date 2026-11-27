// Тесты ведущего: число мафии, раздача ролей, союзники мафии, день
// (порядок речей, голосование, казнь), проверка ответов, подсчёт голосов,
// воспроизводимость протокола по зерну на настоящих нитях.
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "game/voting.hpp"
#include "test_framework.hpp"

#include <map>
#include <sstream>
#include <stdexcept>

using namespace mafia;

namespace {

HostConfig config(int players, std::uint64_t seed, bool execute_day_one = false) {
    HostConfig c;
    c.players = players;
    c.mafia_divisor = 3;
    c.seed = seed;
    c.execute_on_first_day = execute_day_one;
    return c;
}

SharedPtr<Engine> threads() {
    return make_shared_ptr<ThreadEngine>();
}

// Полная демонстрационная партия: протокол целиком.
std::string play(std::uint64_t seed, int days) {
    std::ostringstream out;
    Host host(config(8, seed), threads(), out);
    host.setup();
    for (int d = 0; d < days; ++d) {
        host.run_day();
    }
    host.shutdown();
    host.print_summary();
    return out.str();
}

// Игрок со сценарием: голосует по списку предпочтений (первый доступный),
// на опрос «выгнать всех?» отвечает заданным образом.
class Scripted : public Player {
public:
    Scripted(PlayerSetup setup, RoleKind role, std::vector<PlayerId> preferences, bool agree)
        : Player(std::move(setup), role), preferences_(std::move(preferences)), agree_(agree) {}

protected:
    int target_score(const Request&, Purpose purpose, PlayerId candidate) const override {
        if (purpose == Purpose::Vote) {
            for (std::size_t i = 0; i < preferences_.size(); ++i) {
                if (preferences_[i] == candidate) {
                    return 1000 - static_cast<int>(i);
                }
            }
        }
        return 0;
    }
    std::size_t choose_option(const Request&) override { return agree_ ? 0 : 1; }

private:
    std::vector<PlayerId> preferences_;
    bool agree_;
};

struct Scenario {
    std::vector<std::vector<PlayerId>> preferences;  // по игроку
    std::vector<bool> agree;                         // ответ на опрос по игроку
};

// Один день с голосованием (казнь в первый день включена), 8 игроков.
GameState play_scripted(const Scenario& s, std::string* protocol = nullptr) {
    HostConfig c = config(8, 99, /*execute_day_one=*/true);
    c.make_player = [s](RoleKind role, PlayerSetup setup) {
        const auto id = static_cast<std::size_t>(setup.id);
        return SharedPtr<Player>(
            make_shared_ptr<Scripted>(std::move(setup), role, s.preferences[id], s.agree[id]));
    };
    std::ostringstream out;
    Host host(c, threads(), out);
    host.setup();
    host.run_day();
    host.shutdown();
    if (protocol) {
        *protocol = out.str();
    }
    return host.state();
}

std::vector<PlayerId> dead(const GameState& st) {
    std::vector<PlayerId> out;
    for (const PlayerState& p : st.players) {
        if (!p.alive) {
            out.push_back(p.id);
        }
    }
    return out;
}

int votes_in_round(const GameState& st, int round) {
    int n = 0;
    for (const HistoryEntry& h : st.history) {
        if (const auto* v = std::get_if<VoteEvent>(&h.event); v && v->round == round) {
            ++n;
        }
    }
    return n;
}

template <class E>
int count_events(const GameState& st, int day = -1) {
    int n = 0;
    for (const HistoryEntry& h : st.history) {
        if (std::holds_alternative<E>(h.event) && (day < 0 || h.day == day)) {
            ++n;
        }
    }
    return n;
}

}  // namespace

void mafia_count_formula() {
    CHECK(Host::mafia_count(5, 3) == 1);
    CHECK(Host::mafia_count(6, 3) == 2);
    CHECK(Host::mafia_count(8, 3) == 2);   // 8/3 = 2.67 -> 2
    CHECK(Host::mafia_count(16, 3) == 5);  // 16/3 = 5.33 -> 5
    CHECK(Host::mafia_count(8, 4) == 2);
    CHECK(Host::mafia_count(7, 10) == 1);  // 7/10 = 0 -> поднимаем до 1

    int errors = 0;
    for (auto [n, k] : {std::pair{4, 3}, std::pair{-3, 3}, std::pair{8, 2}, std::pair{8, 0}}) {
        try {
            Host::mafia_count(n, k);
        } catch (const std::invalid_argument&) {
            ++errors;
        }
    }
    CHECK(errors == 4);
}

void setup_deals_roles() {
    for (int n : {5, 8, 16}) {
        std::ostringstream out;
        Host host(config(n, 7), threads(), out);
        host.setup();
        std::map<RoleKind, int> count;
        for (const PlayerState& p : host.state().players) {
            ++count[p.role];
            CHECK(p.alive);
        }
        CHECK(count[RoleKind::Mafioso] == Host::mafia_count(n, 3));
        CHECK(count[RoleKind::Commissar] == 1 && count[RoleKind::Doctor] == 1 && count[RoleKind::Maniac] == 1);
        CHECK(count[RoleKind::Civilian] == n - Host::mafia_count(n, 3) - 3);
        CHECK(out.str().find("Мафия знает своих") != std::string::npos);
        // В стартовом объявлении нет ничьей роли — только состав.
        CHECK(out.str().find(" — Мафиози") == std::string::npos);
        host.shutdown();
    }
}

void shuffle_depends_on_seed() {
    auto roles = [](std::uint64_t seed) {
        std::ostringstream out;
        Host host(config(10, seed), threads(), out);
        host.setup();
        std::vector<RoleKind> r;
        for (const PlayerState& p : host.state().players) {
            r.push_back(p.role);
        }
        host.shutdown();
        return r;
    };
    CHECK(roles(1) == roles(1));
    CHECK(roles(1) != roles(2));
}

void mafia_knows_allies() {
    std::ostringstream out;
    Host host(config(9, 3), threads(), out);  // 3 мафиози
    host.setup();
    for (const PlayerState& p : host.state().players) {
        Briefing b = host.briefing_for(p.id);
        CHECK(b.self == p.id && b.role == p.role);
        if (p.role == RoleKind::Mafioso) {
            CHECK(b.known.size() == 2);  // двое других, себя нет
            for (const KnownIdentity& k : b.known) {
                CHECK(k.is_mafia && k.id != p.id);
                CHECK(host.state().players[static_cast<std::size_t>(k.id)].role == RoleKind::Mafioso);
            }
        } else {
            CHECK(b.known.empty());  // остальным — ничего
        }
    }
    host.shutdown();
}

void first_day_has_no_vote() {
    std::ostringstream out;
    Host host(config(8, 11), threads(), out);
    host.setup();
    host.run_day();
    CHECK(count_events<SpeechEvent>(host.state(), 1) == 8);
    CHECK(count_events<VoteEvent>(host.state(), 1) == 0);
    CHECK(count_events<DeathEvent>(host.state()) == 0);
    host.shutdown();

    std::ostringstream out2;
    Host voting_host(config(8, 11, /*execute_day_one=*/true), threads(), out2);
    voting_host.setup();
    voting_host.run_day();
    // Первый тур — все восемь; при ничьей могут быть и следующие туры.
    CHECK(votes_in_round(voting_host.state(), 1) == 8);
    CHECK(count_events<VoteEvent>(voting_host.state(), 1) >= 8);
    voting_host.shutdown();
}

void starting_speaker_rotates() {
    // Зерно 2026: на второй день выбывает Игрок 3 — как раз тот, чья
    // очередь начинать третий день. Правило должно его пропустить.
    constexpr int kDays = 6;
    const int n = 8;
    std::ostringstream out;
    Host host(config(n, 2026), threads(), out);
    host.setup();
    for (int d = 0; d < kDays; ++d) {
        host.run_day();
    }
    std::vector<PlayerId> first(kDays + 1, -1);   // стартовый говорящий дня d
    std::vector<int> died_on(static_cast<std::size_t>(n), 0);  // день выбывания, 0 — жив
    for (const HistoryEntry& h : host.state().history) {
        if (const auto* s = std::get_if<SpeechEvent>(&h.event); s && first[static_cast<std::size_t>(h.day)] < 0) {
            first[static_cast<std::size_t>(h.day)] = s->speaker;
        }
        if (const auto* d = std::get_if<DeathEvent>(&h.event)) {
            died_on[static_cast<std::size_t>(d->victim)] = h.day;
        }
    }
    CHECK(first[1] == 0);
    int skipped_dead = 0;
    for (int d = 2; d <= kDays; ++d) {
        // Ожидаем: следующий по кругу после вчерашнего стартового, живой к утру дня d.
        PlayerId expected = first[static_cast<std::size_t>(d - 1)];
        do {
            expected = (expected + 1) % n;
            const int died = died_on[static_cast<std::size_t>(expected)];
            if (died != 0 && died < d) {
                ++skipped_dead;
                continue;
            }
            break;
        } while (true);
        CHECK(first[static_cast<std::size_t>(d)] == expected);
        CHECK(first[static_cast<std::size_t>(d)] != first[static_cast<std::size_t>(d - 1)]);
    }
    CHECK(skipped_dead > 0);  // случай «стартовое место выбыло» действительно проверен
    host.shutdown();
}

void votes_are_valid_and_counted() {
    std::ostringstream out;
    Host host(config(8, 21), threads(), out);
    host.setup();
    for (int d = 0; d < 4; ++d) {
        host.run_day();
    }
    const GameState& st = host.state();
    int deaths = 0;
    for (const HistoryEntry& h : st.history) {
        if (const auto* v = std::get_if<VoteEvent>(&h.event)) {
            CHECK(v->target.has_value());
            CHECK(*v->target != v->voter);
        }
        if (const auto* d = std::get_if<DeathEvent>(&h.event)) {
            ++deaths;
            const PlayerState& victim = st.players[static_cast<std::size_t>(d->victim)];
            CHECK(!victim.alive && victim.disclosed);
            CHECK(d->disclosed.is_mafia == (victim.role == RoleKind::Mafioso));
            CHECK(!d->disclosed.role);  // закрытое объявление: роль не раскрыта
        }
    }
    int alive = 0;
    for (const PlayerState& p : st.players) {
        alive += p.alive ? 1 : 0;
    }
    CHECK(alive == 8 - deaths);
    host.shutdown();
}

void tally_rules() {
    VoteTally one = tally_votes({2, 2, 3, std::nullopt, 2});
    CHECK(one.leaders == std::vector<PlayerId>({2}) && one.top == 3 && one.spoiled == 1);
    CHECK(one.counts.front() == std::make_pair(PlayerId{2}, 3));

    VoteTally tie = tally_votes({1, 4, 4, 1, 0});
    CHECK(tie.leaders == std::vector<PlayerId>({1, 4}) && tie.top == 2);

    VoteTally none = tally_votes({std::nullopt, std::nullopt});
    CHECK(none.leaders.empty() && none.spoiled == 2);
}

void decision_rules() {
    auto outcome = [](std::vector<std::optional<PlayerId>> ballots) {
        return decide_vote(tally_votes(ballots));
    };
    // 3-3-1: лидеры не все кандидаты -> переголосование между лидерами.
    VoteDecision d1 = outcome({1, 1, 1, 2, 2, 2, 3});
    CHECK(d1.outcome == VoteOutcome::Revote && d1.players == std::vector<PlayerId>({1, 2}));
    // 5-5-5-3: то же, трое лидеров.
    std::vector<std::optional<PlayerId>> b2;
    for (PlayerId id : {0, 1, 2}) {
        b2.insert(b2.end(), 5, id);
    }
    b2.insert(b2.end(), 3, PlayerId{3});
    VoteDecision d2 = outcome(b2);
    CHECK(d2.outcome == VoteOutcome::Revote && d2.players.size() == 3);
    // 3-3 и 4-4-4: равны все кандидаты -> опрос.
    CHECK(outcome({1, 1, 1, 2, 2, 2}).outcome == VoteOutcome::Poll);
    CHECK(outcome({0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}).outcome == VoteOutcome::Poll);
    // Единственный лидер; ни одного голоса.
    VoteDecision d3 = outcome({4, 4, 1});
    CHECK(d3.outcome == VoteOutcome::Execute && d3.players == std::vector<PlayerId>({4}));
    CHECK(outcome({std::nullopt, std::nullopt}).outcome == VoteOutcome::Nobody);
    // Испорченные бюллетени не делают кандидатом никого.
    CHECK(outcome({1, 2, std::nullopt}).outcome == VoteOutcome::Poll);

    // «Больше половины»: 5 из 8 — да, 4 из 8 — нет, 4 из 7 — да.
    CHECK(kick_all_passes(5, 8));
    CHECK(!kick_all_passes(4, 8));
    CHECK(kick_all_passes(4, 7));
    CHECK(!kick_all_passes(0, 5));
}

void revote_then_execute() {
    // Тур 1: Игрок 2 — 3, Игрок 3 — 3, Игрок 4 — 2 (3-3-2) -> переголосование {1, 2}.
    // Тур 2: голосовавшие за Игрока 4 переходят на Игрока 2 -> 5:3, выбывает Игрок 2.
    Scenario s;
    s.preferences = {{1}, {2}, {1}, {2}, {3, 1}, {3, 1}, {1}, {2}};
    s.agree.assign(8, false);
    std::string protocol;
    GameState st = play_scripted(s, &protocol);

    CHECK(votes_in_round(st, 1) == 8 && votes_in_round(st, 2) == 8);
    CHECK(votes_in_round(st, 3) == 0);
    CHECK(dead(st) == std::vector<PlayerId>({1}));
    CHECK(count_events<PollEvent>(st) == 0);
    for (const HistoryEntry& h : st.history) {
        if (const auto* v = std::get_if<VoteEvent>(&h.event); v && v->round == 2) {
            // Во втором туре — только между лидерами и никогда против себя.
            CHECK(v->target == 1 || v->target == 2);
            CHECK(v->target != v->voter);
        }
    }
    CHECK(protocol.find("Переголосование между ними") != std::string::npos);
    CHECK(protocol.find("тур 2") != std::string::npos);
}

void revote_then_poll() {
    // Тур 1: 3-3-2 -> переголосование {1, 2}. Тур 2: 4-4, равны все -> опрос.
    // Лидеры голосуют друг против друга (Игрок 2 -> Игрок 3 и наоборот).
    Scenario s;
    s.preferences = {{1}, {3, 2}, {3, 1}, {1}, {2}, {2}, {1}, {2}};
    s.agree.assign(8, false);  // опрос не проходит
    GameState st = play_scripted(s);

    CHECK(votes_in_round(st, 1) == 8 && votes_in_round(st, 2) == 8);
    CHECK(count_events<PollEvent>(st) == 8);
    CHECK(dead(st).empty());
}

void poll_kicks_all() {
    // Тур 1: 4-4 (равны все кандидаты) -> опрос; «да» — 5 из 8 -> выбывают оба.
    Scenario s;
    s.preferences = {{1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}};
    s.agree = {true, true, true, true, true, false, false, false};
    std::string protocol;
    GameState st = play_scripted(s, &protocol);

    CHECK(votes_in_round(st, 1) == 8 && votes_in_round(st, 2) == 0);
    CHECK(count_events<PollEvent>(st) == 8);
    CHECK(dead(st) == std::vector<PlayerId>({1, 2}));
    CHECK(count_events<DeathEvent>(st) == 2 && count_events<LastWordEvent>(st) == 2);
    CHECK(protocol.find("Город выгоняет всех") != std::string::npos);
}

void poll_needs_majority() {
    // Тот же расклад, но «да» — ровно половина (4 из 8): этого мало.
    Scenario s;
    s.preferences = {{1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}};
    s.agree = {true, true, true, true, false, false, false, false};
    std::string protocol;
    GameState st = play_scripted(s, &protocol);

    CHECK(count_events<PollEvent>(st) == 8);
    CHECK(dead(st).empty());
    CHECK(protocol.find("этого мало") != std::string::npos);
}

void invalid_votes_rejected() {
    Request req;
    req.id = 5;
    req.kind = RequestKind::Vote;
    req.you.self = 0;
    req.targets = {1, 3};  // живые, кроме голосующего (2 — мёртв)

    Response ok;
    ok.request_id = 5;
    ok.from = 0;
    ok.target = 3;
    CHECK(is_valid_answer(req, ok));

    Response self = ok;
    self.target = 0;
    Response dead = ok;
    dead.target = 2;
    Response stranger = ok;
    stranger.target = 42;
    Response empty = ok;
    empty.target.reset();
    Response forged = ok;
    forged.from = 1;  // ответ «за другого»
    Response stale = ok;
    stale.request_id = 4;  // ответ на прошлый запрос
    for (const Response& bad : {self, dead, stranger, empty, forged, stale}) {
        CHECK(!is_valid_answer(req, bad));
    }
}

void protocol_is_reproducible() {
    const std::string a = play(2026, 4);
    const std::string b = play(2026, 4);
    CHECK(a == b);  // нити не влияют на исход
    CHECK(a != play(2027, 4));
    CHECK(a.find("=== День 4 ===") != std::string::npos);
    CHECK(a.find("=== Итог партии ===") != std::string::npos);
}

int main() {
    std::cout << "Ведущий: подготовка и день\n";
    test::run("число мафии: floor(N/k), не меньше 1", &mafia_count_formula);
    test::run("раздача ролей", &setup_deals_roles);
    test::run("перемешивание зависит от зерна", &shuffle_depends_on_seed);
    test::run("мафия знает союзников, остальные — нет", &mafia_knows_allies);
    test::run("первый день без голосования (настраивается)", &first_day_has_no_vote);
    test::run("стартовый говорящий сдвигается", &starting_speaker_rotates);
    test::run("голоса допустимы, казнь объявлена закрыто", &votes_are_valid_and_counted);
    test::run("подсчёт голосов и ничья", &tally_rules);
    test::run("решение по туру: казнь / переголосование / опрос", &decision_rules);
    test::run("3-3-2 -> переголосование -> казнь", &revote_then_execute);
    test::run("3-3-2 -> переголосование 4-4 -> опрос", &revote_then_poll);
    test::run("4-4 -> опрос, 5 из 8 «да» -> выбывают оба", &poll_kicks_all);
    test::run("опрос: половины «да» мало", &poll_needs_majority);
    test::run("недопустимые голоса отвергаются", &invalid_votes_rejected);
    test::run("протокол воспроизводим по зерну", &protocol_is_reproducible);
    return test::finish();
}
