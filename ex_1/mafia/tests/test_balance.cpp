// Баланс: прогон множества партий с ботами и распределение побед по сторонам.
// Это не проверка правил, а «датчик»: эвристика ботов не должна делать игру
// предрешённой ни для одной стороны.
//
// Допустимые доли (по всем партиям вместе):
//   мирные  — 20..65%   мафия — 20..60%   маньяк — 5..30%   ничьи — не больше 15%.
// Нижняя граница мирных — 20%, а не «честные» 33%: по правилам задания угроз
// на старте 38–50% стола (floor(N/k) мафии + маньяк), и мирным нужно убрать
// их всех. Ориентир — случайная игра ботов (до этапа 11): мирные ~11%.
// 20% — «эвристика заметно лучше случайной», а не «город обязан побеждать».
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "game/victory.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <map>
#include <sstream>
#include <string>

using namespace mafia;

namespace {

constexpr int kSeedsPerSize = 40;
const int kSizes[] = {6, 8, 10, 12, 16};

std::string winner_of_game(int players, std::uint64_t seed) {
    HostConfig c;
    c.players = players;
    c.seed = seed;
    std::ostringstream out;  // протокол не нужен — только итог
    Host host(c, make_shared_ptr<ThreadEngine>(), out);
    host.setup();
    host.play();
    host.shutdown();
    return std::string(winner_code(host.state().winner));
}

double share(int part, int total) {
    return total == 0 ? 0.0 : 100.0 * part / total;
}

}  // namespace

void win_distribution() {
    const char* sides[] = {"civilians", "mafia", "maniac", "draw"};
    std::map<std::string, int> total;
    int games = 0;

    // Заголовок — готовой строкой: printf выравнивает по байтам, а кириллица — 2 байта на букву.
    std::printf("\n      N        мирные      мафия     маньяк      ничья\n");
    for (int n : kSizes) {
        std::map<std::string, int> row;
        for (std::uint64_t seed = 1; seed <= kSeedsPerSize; ++seed) {
            const std::string w = winner_of_game(n, 1000 + seed);
            ++row[w];
            ++total[w];
            ++games;
        }
        std::printf("      %-4d", n);
        for (const char* s : sides) {
            std::printf(" %9.0f%%", share(row[s], kSeedsPerSize));
        }
        std::printf("\n");
    }
    std::printf("      %-4s", "все");
    for (const char* s : sides) {
        std::printf(" %9.0f%%", share(total[s], games));
    }
    std::printf("   (%d партий)\n", games);
    std::fflush(stdout);

    const double civ = share(total["civilians"], games);
    const double maf = share(total["mafia"], games);
    const double man = share(total["maniac"], games);
    const double draw = share(total["draw"], games);
    CHECK(civ >= 20 && civ <= 65);
    CHECK(maf >= 20 && maf <= 60);
    CHECK(man >= 5 && man <= 30);  // одиночка против всех не должен выигрывать как команда
    CHECK(draw <= 15);
}

int main() {
    std::cout << "Баланс сторон";
    test::run("распределение побед", &win_distribution);
    return test::finish();
}
