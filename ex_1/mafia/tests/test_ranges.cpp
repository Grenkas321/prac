// Свойства ranges, на которые опирается код ведущего и подсчёта голосов:
// представления ленивые (ничего не вычисляют заранее), не копируют данные
// и останавливаются, как только ответ известен.
#include "game/state.hpp"
#include "game/voting.hpp"
#include "test_framework.hpp"

#include <algorithm>  // алгоритмы std::ranges::* — здесь, а не в <ranges>
#include <iterator>
#include <ranges>
#include <vector>

using namespace mafia;

namespace {

std::vector<PlayerState> table(int n) {
    std::vector<PlayerState> players;
    for (PlayerId i = 0; i < n; ++i) {
        PlayerState p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        players.push_back(p);
    }
    return players;
}

}  // namespace

// Та же цепочка, что в Host::alive_except: это представление, а не контейнер.
using AliveIds = decltype(std::declval<std::vector<PlayerState>&>() | std::views::filter(&PlayerState::alive) |
                          std::views::transform(&PlayerState::id));
static_assert(std::ranges::view<AliveIds>);
static_assert(std::ranges::forward_range<AliveIds>);

// take_while — не common_range: конец у него — страж другого типа.
// Поэтому в tally_votes лидеры копируются через ranges::copy, а не assign(begin, end).
using Leaders = decltype(std::declval<std::vector<std::pair<int, int>>&>() |
                         std::views::take_while([](const auto&) { return true; }));
static_assert(!std::ranges::common_range<Leaders>);

void view_does_not_copy() {
    std::vector<PlayerState> players = table(6);
    auto alive = players | std::views::filter(&PlayerState::alive) | std::views::transform(&PlayerState::id);
    CHECK(std::ranges::distance(alive) == 6);
    // Меняем данные ПОСЛЕ создания представления — оно видит изменение:
    // значит, оно ничего не скопировало, а смотрит в исходный вектор.
    players[2].alive = false;
    players[4].alive = false;
    CHECK(std::ranges::distance(alive) == 4);
    CHECK(std::ranges::equal(alive, std::vector<PlayerId>{0, 1, 3, 5}));
}

void view_is_lazy_and_stops_early() {
    std::vector<PlayerState> players = table(10);
    for (auto& p : players) {
        p.alive = p.id >= 3;  // первые трое мертвы
    }
    int checks = 0;
    auto alive = players | std::views::filter([&](const PlayerState& p) {
                     ++checks;
                     return p.alive;
                 });
    CHECK(checks == 0);  // создание представления ничего не вычисляет

    // «Следующий живой» (как в Host::next_alive_after): берём первый —
    // проверены только места 0..3, остальные шесть не трогались.
    auto first = std::ranges::begin(alive);
    CHECK(first->id == 3);
    CHECK(checks == 4);

    // any_of тоже останавливается на первом совпадении.
    int seen = 0;
    const bool any = std::ranges::any_of(players, [&](const PlayerState& p) {
        ++seen;
        return p.alive;
    });
    CHECK(any && seen == 4);
}

void take_while_stops_at_first_smaller() {
    // Как лидеры в tally_votes: отсортированный по убыванию список,
    // take_while берёт префикс с максимумом и дальше не смотрит.
    std::vector<std::pair<int, int>> counts = {{4, 3}, {1, 3}, {2, 1}, {0, 1}, {5, 1}};
    int looked = 0;
    auto leaders = counts | std::views::take_while([&](const auto& c) {
                       ++looked;
                       return c.second == 3;
                   }) |
                   std::views::keys;
    std::vector<int> got;
    std::ranges::copy(leaders, std::back_inserter(got));
    CHECK(got == std::vector<int>({4, 1}));
    CHECK(looked == 3);  // два лидера + один, на котором остановились
}

void tally_matches_old_behaviour() {
    // Переписанный на ranges подсчёт даёт то же, что и прежний цикл.
    VoteTally t = tally_votes({2, 5, 2, std::nullopt, 5, 1, std::nullopt});
    CHECK(t.spoiled == 2);
    CHECK(t.top == 2);
    CHECK(t.leaders == std::vector<PlayerId>({2, 5}));  // при равенстве — по номеру
    CHECK(t.counts.back() == std::make_pair(PlayerId{1}, 1));
}

int main() {
    std::cout << "Ranges\n";
    test::run("представление не копирует данные", &view_does_not_copy);
    test::run("ленивость: ничего заранее, остановка на первом", &view_is_lazy_and_stops_early);
    test::run("take_while останавливается на первом меньшем", &take_while_stops_at_first_smaller);
    test::run("подсчёт голосов на ranges — как раньше", &tally_matches_old_behaviour);
    return test::finish();
}
