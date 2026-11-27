// Массовый тест: много партий целиком на разных N, k, зёрнах, режимах и
// наборах ролей — каждая на ОБОИХ движках.
//
// Для каждой партии проверяется:
//   * нити и корутины дают побайтно одинаковый протокол (и строку RESULT);
//   * повтор того же зерна даёт ту же партию (выборочно);
//   * партия закончилась, итог согласован с составом живых (judge),
//     список выживших в RESULT совпадает с состоянием, день — в пределе.
// По базовым партиям — распределение побед: ни одна сторона не предрешена.
//
// Группы партий:
//   база      — только обязательные роли: N 5..30, k 3..5, казнь в 1-й день вкл/выкл;
//   доп. роли — случайные допустимые наборы из 35 дополнительных ролей;
//   конфиги   — config/example.yaml и config/all_roles.yaml как есть;
//   ИИ        — ИИ-игроки через локальный поддельный сервис модели.
//
// Объём: MAFIA_MASS=<множитель> (по умолчанию 1; 0.2 — быстрый прогон).
#include "ai/ai_advisor.hpp"
#include "ai/http_client.hpp"
#include "cli/config.hpp"
#include "core/random.hpp"
#include "engine/console_engine.hpp"
#include "engine/coro_engine.hpp"
#include "engine/thread_engine.hpp"
#include "fake_llm_server.hpp"
#include "game/host.hpp"
#include "game/victory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace mafia;

namespace {

double scale() {
    const char* s = std::getenv("MAFIA_MASS");
    const double v = s ? std::atof(s) : 1.0;
    return v > 0 ? v : 1.0;
}

int scaled(int n) {
    return std::max(1, static_cast<int>(n * scale()));
}

struct Setup {
    int players = 8;
    int divisor = 3;
    std::uint64_t seed = 1;
    bool execute_day_one = false;
    std::vector<RoleKind> extra;
    int ai = 0;
    std::string ai_url;
    bool open = false;                  // открытые объявления
    std::optional<PlayerId> human;      // место пользователя (--interactive)
};

std::string describe(const Setup& s) {
    std::string d = "N=" + std::to_string(s.players) + " k=" + std::to_string(s.divisor) +
                    " seed=" + std::to_string(s.seed) + (s.execute_day_one ? " казнь-с-1-дня" : "") +
                    (s.open ? " открытые" : "") + (s.human ? " пользователь=" + std::to_string(*s.human + 1) : "");
    if (!s.extra.empty()) {
        d += " доп.ролей=" + std::to_string(s.extra.size());
    }
    if (s.ai > 0) {
        d += " ИИ=" + std::to_string(s.ai);
    }
    return d;
}

struct Outcome {
    std::string protocol;
    std::string result;  // строка RESULT
    std::string winner;
    bool consistent = false;
    std::string why;  // почему не согласован
};

Outcome play(const Setup& s, bool coro) {
    HostConfig c;
    c.players = s.players;
    c.mafia_divisor = s.divisor;
    c.seed = s.seed;
    c.execute_on_first_day = s.execute_day_one;
    c.open_announcements = s.open;
    c.human_seat = s.human;
    c.extra_roles = s.extra;
    if (s.ai > 0) {
        auto cfg = make_shared_ptr<AiConfig>();
        cfg->base_url = s.ai_url;
        cfg->calls_per_round = 1000;  // без отказов по лимиту: порядок занятия зависит от нитей
        const SharedPtr<const AiConfig> config = cfg;
        const SharedPtr<HttpClient> client = make_shared_ptr<CurlClient>();
        const auto budget = make_shared_ptr<AiBudget>(cfg->calls_per_round);
        c.ai_players = s.ai;
        c.make_advisor = [=](PlayerId, int k) -> SharedPtr<Advisor> {
            return make_shared_ptr<AiAdvisor>(config, client, budget, SharedPtr<AiDialogLog>(),
                                              std::string(personality_for(k)));
        };
    }
    SharedPtr<Engine> engine;
    if (coro) {
        engine = make_shared_ptr<CoroEngine>();
    } else {
        engine = make_shared_ptr<ThreadEngine>();
    }
    std::ostringstream out;
    // Пользователь: ввод «наугад» — числа по кругу и обвинения. Он один и тот
    // же на обоих движках, поэтому и партии должны совпасть.
    std::istringstream input;
    if (s.human) {
        std::string lines;
        for (int i = 0; i < 6000; ++i) {
            lines += (i % 7 == 6 ? "обвинить " : "") + std::to_string(i % s.players + 1) + "\n";
        }
        input.str(lines);
        engine = make_shared_ptr<ConsoleEngine>(engine, *s.human, input, out);
    }
    Host host(c, engine, out);
    host.setup();
    host.play();
    host.shutdown();

    Outcome o;
    // Протокол без строки «Исполнение: …» — единственной, где движок называет себя.
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);) {
        if (!line.starts_with("Исполнение: ")) {
            o.protocol += line + '\n';
        }
    }
    const auto pos = o.protocol.find("RESULT ");
    o.result = pos == std::string::npos ? "" : o.protocol.substr(pos, o.protocol.find('\n', pos) - pos);

    // Согласованность итога с состоянием.
    const GameState& st = host.state();
    o.winner = std::string(winner_code(st.winner));
    const AliveCount alive = count_alive(st);
    const Verdict v = judge(alive);
    std::string survivors;
    for (const PlayerState& p : st.players) {
        if (p.alive) {
            survivors += (survivors.empty() ? "" : ",") + std::to_string(p.id + 1);
        }
    }
    const bool day_limit = o.result.find("reason=day_limit") != std::string::npos;
    if (!st.finished) {
        o.why = "партия не закончена";
    } else if (o.result.empty()) {
        o.why = "нет строки RESULT";
    } else if (o.result.find("survivors=" + (survivors.empty() ? std::string("none") : survivors)) ==
               std::string::npos) {
        o.why = "выжившие в RESULT не совпадают с состоянием";
    } else if (st.day > c.max_days) {
        o.why = "день за пределом";
    } else if (day_limit ? st.winner.has_value() : winner_of(v) != st.winner || v == Verdict::Continue) {
        o.why = "итог не согласован с составом живых (мирные " + std::to_string(alive.civilians) + ", мафия " +
                std::to_string(alive.mafia) + ", одиночки " + std::to_string(alive.loners) + ")";
    } else {
        o.consistent = true;
    }
    return o;
}

struct Tally {
    int games = 0;
    int mismatches = 0;    // нити != корутины
    int irreproducible = 0;
    int inconsistent = 0;
    std::map<std::string, int> wins;
    std::map<int, std::map<std::string, int>> wins_by_n;
};

// Сыграть на обоих движках (и иногда повторить), всё проверить и учесть.
void run(const Setup& s, Tally& t, bool repeat) {
    const Outcome th = play(s, false);
    const Outcome co = play(s, true);
    t.games += 2;
    if (th.protocol != co.protocol && ++t.mismatches <= 5) {
        // Первая различающаяся строка — чтобы сразу видеть, где разошлись.
        std::istringstream a(th.protocol), b(co.protocol);
        std::string la, lb;
        while (std::getline(a, la) && std::getline(b, lb) && la == lb) {
        }
        std::cout << "\n      расхождение движков: " << describe(s) << "\n        нити:     " << la
                  << "\n        корутины: " << lb;
    }
    if (repeat && play(s, false).protocol != th.protocol) {
        ++t.irreproducible;
        std::cout << "\n      зерно не воспроизводит партию: " << describe(s);
    }
    for (const Outcome* o : {&th, &co}) {
        if (!o->consistent) {
            ++t.inconsistent;
            std::cout << "\n      " << o->why << ": " << describe(s) << "\n        " << o->result;
        }
    }
    ++t.wins[th.winner];
    ++t.wins_by_n[s.players][th.winner];
}

double share(int part, int total) {
    return total == 0 ? 0.0 : 100.0 * part / total;
}

void print_distribution(const char* title, const Tally& t, bool by_n) {
    const char* sides[] = {"civilians", "mafia", "maniac", "draw"};
    std::printf("\n      %s\n      N        мирные      мафия     маньяк      ничья   партий\n", title);
    if (by_n) {
        for (const auto& [n, row] : t.wins_by_n) {
            int total = 0;
            for (const auto& [w, k] : row) {
                total += k;
            }
            std::printf("      %-4d", n);
            for (const char* s : sides) {
                std::printf(" %9.0f%%", share(row.count(s) ? row.at(s) : 0, total));
            }
            std::printf(" %8d\n", total);
        }
    }
    const int total = t.games / 2;
    std::printf("      %-4s", "все");
    for (const char* s : sides) {
        std::printf(" %9.0f%%", share(t.wins.count(s) ? t.wins.at(s) : 0, total));
    }
    std::printf(" %8d\n", total);
    std::fflush(stdout);
}

void check_tally(const Tally& t) {
    CHECK(t.games > 0);
    CHECK(t.mismatches == 0);
    CHECK(t.irreproducible == 0);
    CHECK(t.inconsistent == 0);
}

int win(const Tally& t, const char* side) {
    return t.wins.count(side) ? t.wins.at(side) : 0;
}

// ---------------------------------------------------------------------------

void base_roles() {
    Tally t;
    const int seeds = scaled(4);
    int configs = 0;
    for (int n : {5, 6, 7, 8, 9, 10, 12, 14, 16, 20, 25, 30}) {
        for (int k : {3, 4, 5}) {
            for (bool day_one : {false, true}) {
                for (int i = 0; i < seeds; ++i) {
                    Setup s;
                    s.players = n;
                    s.divisor = k;
                    s.execute_day_one = day_one;
                    s.open = i % 2 == 1;  // половина партий — с открытыми объявлениями
                    s.seed = 10'000 + static_cast<std::uint64_t>(n * 1000 + k * 100 + i * 2 + day_one);
                    run(s, t, configs % 7 == 0);
                    ++configs;
                }
            }
        }
    }
    std::printf("\n      база: %d конфигураций, %d партий", configs, t.games + configs / 7 + 1);
    print_distribution("победы, только обязательные роли:", t, true);
    check_tally(t);
    // Разумное распределение: у каждой стороны есть шансы, никто не предрешён.
    const int games = t.games / 2;
    CHECK(share(win(t, "civilians"), games) >= 15 && share(win(t, "civilians"), games) <= 65);
    CHECK(share(win(t, "mafia"), games) >= 20 && share(win(t, "mafia"), games) <= 65);
    CHECK(share(win(t, "maniac"), games) >= 5 && share(win(t, "maniac"), games) <= 35);
    CHECK(share(win(t, "draw"), games) <= 15);
}

std::vector<RoleKind> extra_roles() {
    std::vector<RoleKind> out;
    for (RoleKind r : kAllRoles) {
        if (std::ranges::find(kBaseRoles, r) == std::ranges::end(kBaseRoles)) {
            out.push_back(r);
        }
    }
    return out;
}

void random_role_sets() {
    const std::vector<RoleKind> pool = extra_roles();
    CHECK(pool.size() == 35);
    Rng rng(20'261'003);
    Tally t;
    std::map<RoleKind, int> dealt;
    const int configs = scaled(150);
    for (int i = 0; i < configs; ++i) {
        Setup s;
        s.players = static_cast<int>(rng.uniform_int(8, 40));
        s.divisor = static_cast<int>(rng.uniform_int(3, 5));
        s.execute_day_one = rng.chance(0.3);
        s.open = rng.chance(0.5);
        s.seed = 50'000 + static_cast<std::uint64_t>(i);
        const int mafia = Host::mafia_count(s.players, s.divisor);
        int mafia_slots = mafia - 1;             // хотя бы один обычный мафиози
        int civil_slots = s.players - mafia - 3;  // минус комиссар, доктор, маньяк
        std::vector<RoleKind> shuffled = pool;
        rng.shuffle(shuffled);
        const int want = static_cast<int>(rng.uniform_int(1, 8));
        for (RoleKind r : shuffled) {
            if (static_cast<int>(s.extra.size()) == want) {
                break;
            }
            int& slots = team_of(r) == Team::Mafia ? mafia_slots : civil_slots;
            if (slots > 0) {
                --slots;
                s.extra.push_back(r);
                ++dealt[r];
            }
        }
        run(s, t, i % 10 == 0);
    }
    std::printf("\n      доп. роли: %d конфигураций, %d партий, задействовано ролей: %zu из 35", configs,
                t.games, dealt.size());
    print_distribution("победы, случайные наборы доп. ролей:", t, false);
    check_tally(t);
    CHECK(dealt.size() == 35 || scale() < 1);  // каждая роль сыграла хотя бы раз
    CHECK(share(win(t, "draw"), t.games / 2) <= 25);
}

void config_files() {
    Tally t;
    struct File {
        const char* path;
        int players;
    };
    for (const File& f : {File{"config/example.yaml", 12}, File{"config/all_roles.yaml", 40}}) {
        const GameConfig cfg = load_config(f.path);
        for (int i = 0; i < scaled(10); ++i) {
            Setup s;
            s.players = f.players;
            s.divisor = cfg.mafia_divisor.value_or(3);
            s.extra = cfg.extra_roles;
            s.seed = 70'000 + static_cast<std::uint64_t>(i);
            run(s, t, i == 0);
        }
    }
    std::printf("\n      конфиги: %d партий", t.games);
    check_tally(t);
}

void ai_players() {
    fake::Server server;
    Tally t;
    for (int i = 0; i < scaled(8); ++i) {
        Setup s;
        s.players = 6 + i % 6;
        s.seed = 90'000 + static_cast<std::uint64_t>(i);
        s.ai = 1 + i % 3;
        s.ai_url = server.url();
        run(s, t, false);
    }
    std::printf("\n      ИИ: %d партий, обращений к поддельной модели: %zu", t.games, server.count());
    check_tally(t);
    CHECK(server.count() > 0);
}

void user_at_table() {
    Tally t;
    for (int i = 0; i < scaled(12); ++i) {
        Setup s;
        s.players = 6 + i % 7;
        s.seed = 95'000 + static_cast<std::uint64_t>(i);
        s.human = static_cast<PlayerId>(i % s.players);
        s.open = i % 2 == 0;
        run(s, t, i % 4 == 0);
    }
    std::printf("\n      пользователь: %d партий", t.games);
    check_tally(t);
}

}  // namespace

int main() {
    std::cout << "Массовый тест (MAFIA_MASS=" << scale() << ")\n";
    test::run("обязательные роли: N, k, режимы, оба движка", &base_roles);
    test::run("случайные наборы доп. ролей, оба движка", &random_role_sets);
    test::run("конфиги из config/, оба движка", &config_files);
    test::run("ИИ-игроки через поддельный сервис, оба движка", &ai_players);
    test::run("пользователь за столом (скриптованный ввод), оба движка", &user_at_table);
    return test::finish();
}
