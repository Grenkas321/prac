// Тесты конца игры: таблица условий победы (чистая функция) и полные партии
// на настоящих нитях — на разных N и зёрнах партия доходит до конца, итог
// печатается, машиночитаемая строка совпадает с состоянием.
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "game/victory.hpp"
#include "test_framework.hpp"

#include <map>
#include <set>
#include <sstream>

using namespace mafia;

namespace {

AliveCount alive(int civilians, int mafia, int maniac) {
    AliveCount a;
    a.civilians = civilians;
    a.mafia = mafia;
    a.loners = maniac;
    return a;
}

struct Game {
    GameState state;
    std::string protocol;
};

Game play_full(int players, std::uint64_t seed, int max_days = 317) {
    HostConfig c;
    c.players = players;
    c.seed = seed;
    c.max_days = max_days;
    std::ostringstream out;
    Host host(c, make_shared_ptr<ThreadEngine>(), out);
    host.setup();
    host.play();
    host.shutdown();
    return Game{host.state(), out.str()};
}

// Значение ключа из строки «RESULT key=value ...».
std::string result_field(const std::string& protocol, const std::string& key) {
    const std::size_t line = protocol.find("RESULT ");
    if (line == std::string::npos) {
        return "?";
    }
    const std::size_t eol = protocol.find('\n', line);
    const std::string text = protocol.substr(line, eol - line) + " ";
    const std::size_t at = text.find(" " + key + "=");
    if (at == std::string::npos) {
        return "?";
    }
    const std::size_t from = at + key.size() + 2;
    return text.substr(from, text.find(' ', from) - from);
}

}  // namespace

void judge_table() {
    // (мирные, мафия, маньяк) -> вердикт
    struct Row {
        AliveCount a;
        Verdict v;
    };
    const Row rows[] = {
        // --- двое за столом ---
        {alive(2, 0, 0), Verdict::CiviliansWin},   // мирный + мирный
        {alive(1, 1, 0), Verdict::MafiaParity},    // мирный + мафиози: 1 = 1
        {alive(1, 0, 1), Verdict::ManiacWin},      // мирный + маньяк: один на один
        {alive(0, 1, 1), Verdict::Continue},       // мафиози + маньяк: 1 = 1, но маньяк жив
        {alive(0, 2, 0), Verdict::MafiaMajority},  // двое мафиози
        // --- трое за столом ---
        {alive(3, 0, 0), Verdict::CiviliansWin},
        {alive(2, 1, 0), Verdict::Continue},       // 1 < 2
        {alive(1, 2, 0), Verdict::MafiaMajority},  // 2 > 1
        {alive(2, 0, 1), Verdict::Continue},       // маньяк против двоих мирных
        {alive(1, 1, 1), Verdict::Continue},       // 1 < 1 + 1
        {alive(0, 2, 1), Verdict::MafiaMajority},  // 2 > 1
        {alive(0, 3, 0), Verdict::MafiaMajority},
        // --- крайние случаи ---
        {alive(0, 0, 0), Verdict::Draw},           // все погибли одной ночью
        {alive(0, 0, 1), Verdict::ManiacWin},      // маньяк остался один
        {alive(0, 1, 0), Verdict::MafiaMajority},
        {alive(2, 2, 0), Verdict::MafiaParity},    // 2 = 2 — паритет
        {alive(1, 2, 1), Verdict::Continue},       // 2 = 1 + 1, маньяк жив — играем дальше
        {alive(4, 2, 0), Verdict::Continue},
    };
    for (const Row& r : rows) {
        CHECK(judge(r.a) == r.v);
    }
    CHECK(winner_of(Verdict::MafiaParity) == Team::Mafia);
    CHECK(winner_of(Verdict::ManiacWin) == Team::Loner);
    CHECK(!winner_of(Verdict::Draw) && !winner_of(Verdict::Continue));
    CHECK(winner_code(std::nullopt) == "draw" && winner_code(Team::Civilians) == "civilians");
}

void games_reach_the_end() {
    std::map<std::string, int> winners;
    int games = 0;
    for (int n : {5, 6, 7, 8, 10, 12, 16}) {
        for (std::uint64_t seed = 1; seed <= 8; ++seed) {
            const Game g = play_full(n, seed);
            ++games;
            CHECK(g.state.finished);
            CHECK(g.state.phase == Phase::Finished);

            // Итог в состоянии согласован с составом живых.
            const Verdict v = judge(count_alive(g.state));
            const std::string winner = result_field(g.protocol, "winner");
            const std::string reason = result_field(g.protocol, "reason");
            if (reason == "day_limit") {
                CHECK(v == Verdict::Continue && !g.state.winner);
            } else {
                CHECK(v != Verdict::Continue);
                CHECK(g.state.winner == winner_of(v));
            }
            CHECK(winner == winner_code(g.state.winner));
            CHECK(result_field(g.protocol, "days") == std::to_string(g.state.day));
            CHECK(result_field(g.protocol, "players") == std::to_string(n));
            CHECK(g.protocol.find("=== Итог партии ===") != std::string::npos);
            ++winners[winner];

            // После объявления конца — ни одной речи и ни одного голоса.
            bool over = false;
            for (const HistoryEntry& h : g.state.history) {
                if (over) {
                    CHECK(!std::holds_alternative<SpeechEvent>(h.event) &&
                          !std::holds_alternative<VoteEvent>(h.event));
                }
                if (const auto* a = std::get_if<AnnouncementEvent>(&h.event);
                    a && a->text.rfind("Игра окончена", 0) == 0) {
                    over = true;
                }
            }
            CHECK(over);
        }
    }
    CHECK(games == 56);
    // На 56 партиях встречаются победы и мирных, и мафии.
    CHECK(winners["civilians"] > 0 && winners["mafia"] > 0);
    std::cout << "\n      победы по 56 партиям:";
    for (const auto& [w, k] : winners) {
        std::cout << " " << w << "=" << k;
    }
}

void day_limit_is_a_draw() {
    // Предел 1 день: в первый день никого не казнят, ночи не будет — ничья по пределу.
    const Game g = play_full(8, 3, /*max_days=*/1);
    CHECK(g.state.finished && !g.state.winner);
    CHECK(g.state.day == 1);
    CHECK(result_field(g.protocol, "winner") == "draw");
    CHECK(result_field(g.protocol, "reason") == "day_limit");
    CHECK(result_field(g.protocol, "survivors") == "1,2,3,4,5,6,7,8");
    CHECK(count_alive(g.state).civilians + count_alive(g.state).mafia + count_alive(g.state).loners == 8);
}

void summary_table_lists_everyone() {
    const Game g = play_full(8, 2026);
    // Ищем только внутри итога: выше по протоколу «Игрок N» встречается в речах и голосах.
    const std::string summary = g.protocol.substr(g.protocol.find("=== Итог партии ==="));
    for (const PlayerState& p : g.state.players) {
        // У каждого в таблице — имя, роль и исход.
        const std::size_t row = summary.find("\n  " + p.name + " ");
        CHECK(row != std::string::npos);
        const std::string line = summary.substr(row + 1, summary.find('\n', row + 1) - row - 1);
        CHECK(line.find(std::string(role_name(p.role))) != std::string::npos);
        CHECK(line.find(p.alive ? "жив" : (line.find("казнён") != std::string::npos ? "казнён" : "убит ночью")) !=
              std::string::npos);
    }
    // Выжившие в строке RESULT — ровно живые.
    std::string expected;
    for (const PlayerState& p : g.state.players) {
        if (p.alive) {
            expected += (expected.empty() ? "" : ",") + std::to_string(p.id + 1);
        }
    }
    CHECK(result_field(g.protocol, "survivors") == (expected.empty() ? "none" : expected));
}

void result_is_reproducible() {
    const Game a = play_full(10, 42);
    const Game b = play_full(10, 42);
    CHECK(a.protocol == b.protocol);
    CHECK(result_field(a.protocol, "seed") == "42");
}

int main() {
    std::cout << "Конец игры\n";
    test::run("таблица условий победы (2 и 3 игрока, крайние случаи)", &judge_table);
    test::run("партии доходят до конца: N = 5..16, по 8 зёрен", &games_reach_the_end);
    test::run("предел дней — ничья", &day_limit_is_a_draw);
    test::run("итоговая таблица и строка RESULT", &summary_table_lists_everyone);
    test::run("итог воспроизводим по зерну", &result_is_reproducible);
    return test::finish();
}
