// Тесты Rng: совпадение с эталонными значениями алгоритмов, воспроизводимость
// по зерну, независимость потоков, корректность распределений.
#include "core/random.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <set>
#include <vector>

using mafia::Rng;

namespace {

std::vector<std::uint64_t> take(Rng rng, int n) {
    std::vector<std::uint64_t> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(rng.next());
    }
    return out;
}

// Смесь всех методов — «партия в миниатюре».
std::vector<std::int64_t> mixed_session(std::uint64_t seed) {
    Rng rng(seed);
    std::vector<std::int64_t> out;
    std::vector<int> deck(10);
    std::iota(deck.begin(), deck.end(), 0);
    rng.shuffle(deck);
    out.insert(out.end(), deck.begin(), deck.end());
    out.push_back(rng.uniform_int(-5, 5));
    out.push_back(rng.pick(deck));
    out.push_back(rng.chance(0.3) ? 1 : 0);
    out.push_back(static_cast<std::int64_t>(rng.uniform_real() * 1e9));
    return out;
}

}  // namespace

void reference_splitmix64() {
    // Эталон: splitmix64 с начальным состоянием 0.
    std::uint64_t state = 0;
    CHECK(mafia::splitmix64(state) == 0xe220a8397b1dcdafULL);
    CHECK(mafia::splitmix64(state) == 0x6e789e6aa1b965f4ULL);
    CHECK(mafia::splitmix64(state) == 0x06c45d188009454fULL);
}

void reference_xoshiro() {
    // Эталон: xoshiro256** с состоянием {1, 2, 3, 4}.
    Rng rng = Rng::from_state({1, 2, 3, 4});
    CHECK(rng.next() == 11520ULL);
    CHECK(rng.next() == 0ULL);
    CHECK(rng.next() == 1509978240ULL);
    CHECK(rng.next() == 1215971899390074240ULL);

    bool thrown = false;
    try {
        Rng::from_state({0, 0, 0, 0});
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
}

void same_seed_same_sequence() {
    CHECK(take(Rng(2026), 1000) == take(Rng(2026), 1000));
    CHECK(mixed_session(2026) == mixed_session(2026));
    CHECK(take(Rng(0), 100) == take(Rng(0), 100));
}

void different_seeds_differ() {
    CHECK(take(Rng(2026), 100) != take(Rng(2027), 100));
    CHECK(mixed_session(1) != mixed_session(2));

    // Ни одно из первых чисел двух соседних зёрен не совпадает.
    std::vector<std::uint64_t> a = take(Rng(1), 1000);
    std::vector<std::uint64_t> b = take(Rng(2), 1000);
    std::set<std::uint64_t> sa(a.begin(), a.end());
    int common = 0;
    for (std::uint64_t x : b) {
        common += static_cast<int>(sa.count(x));
    }
    CHECK(common == 0);
}

void golden_values() {
    // Зафиксированные значения: если поменять алгоритм или распределение,
    // старые партии с тем же зерном перестанут воспроизводиться — тест упадёт.
    CHECK(Rng(2026).next() == 0x92e011592e98ae15ULL);
    std::vector<int> deck(10);
    std::iota(deck.begin(), deck.end(), 0);
    Rng rng(2026);
    rng.shuffle(deck);
    CHECK(deck == (std::vector<int>{3, 4, 8, 5, 6, 0, 1, 2, 7, 9}));
}

void streams_are_independent() {
    const std::uint64_t seed = 42;
    // Поток определяется только (seed, index).
    CHECK(take(Rng::stream(seed, 3), 50) == take(Rng::stream(seed, 3), 50));
    // Поток 0 — это сам Rng(seed).
    CHECK(take(Rng::stream(seed, 0), 50) == take(Rng(seed), 50));
    // Потоки одного зерна различны.
    std::set<std::vector<std::uint64_t>> distinct;
    for (std::uint64_t k = 0; k < 12; ++k) {
        distinct.insert(take(Rng::stream(seed, k), 20));
    }
    CHECK(distinct.size() == 12);

    // Ловушка «seed + id»: у соседних партий потоки не должны совпадать.
    // При seed + id игрок 1 партии 42 и игрок 0 партии 43 получили бы одно зерно 43.
    CHECK(take(Rng::stream(42, 2), 20) != take(Rng::stream(43, 1), 20));
    CHECK(take(Rng(42 + 1), 20) == take(Rng(43 + 0), 20));  // а так и было бы

    // Чередование чтений не влияет на результат: у потоков нет общего состояния.
    Rng p1 = Rng::stream(seed, 1);
    Rng p2 = Rng::stream(seed, 2);
    std::vector<std::uint64_t> interleaved;
    for (int i = 0; i < 20; ++i) {
        interleaved.push_back(p1.next());
        p2.next();
        p2.next();  // второй игрок «думает» дольше
    }
    CHECK(interleaved == take(Rng::stream(seed, 1), 20));
}

void ranges_and_bounds() {
    Rng rng(7);
    std::set<std::int64_t> seen;
    for (int i = 0; i < 10'000; ++i) {
        std::int64_t v = rng.uniform_int(-3, 3);
        CHECK(v >= -3 && v <= 3);
        seen.insert(v);
    }
    CHECK(seen.size() == 7);  // обе границы включены, все значения встречаются

    CHECK(rng.uniform_int(5, 5) == 5);
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    rng.uniform_int(kMin, kMax);  // полный диапазон: без переполнения
    CHECK(rng.uniform_int(kMax - 1, kMax) >= kMax - 1);

    for (int i = 0; i < 10'000; ++i) {
        double x = rng.uniform_real();
        CHECK(x >= 0.0 && x < 1.0);
        double y = rng.uniform_real(2.0, 3.0);
        CHECK(y >= 2.0 && y < 3.0);
    }

    bool thrown = false;
    try {
        rng.uniform_int(3, 2);
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
}

void unbiased_small_range() {
    // 60 000 бросков кости: каждая грань ~10 000; допуск 5% — с большим запасом.
    Rng rng(123);
    int counts[6] = {};
    for (int i = 0; i < 60'000; ++i) {
        ++counts[rng.below(6)];
    }
    for (int c : counts) {
        CHECK(c > 9'500 && c < 10'500);
    }
}

void coin() {
    Rng rng(99);
    int heads = 0;
    for (int i = 0; i < 10'000; ++i) {
        CHECK(!rng.chance(0.0));
        CHECK(rng.chance(1.0));
        heads += rng.chance(0.25) ? 1 : 0;
    }
    CHECK(heads > 2'300 && heads < 2'700);
}

void pick_and_shuffle() {
    Rng rng(5);
    std::vector<int> v = {10, 20, 30};
    std::set<int> picked;
    for (int i = 0; i < 300; ++i) {
        picked.insert(rng.pick(v));
    }
    CHECK(picked == std::set<int>({10, 20, 30}));

    std::vector<int> deck(52);
    std::iota(deck.begin(), deck.end(), 0);
    std::vector<int> shuffled = deck;
    rng.shuffle(shuffled);
    CHECK(shuffled != deck);
    std::vector<int> sorted = shuffled;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == deck);  // перестановка: ничего не потеряно и не повторено

    bool thrown = false;
    try {
        rng.pick(std::vector<int>{});
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
}

int main() {
    std::cout << "Rng (xoshiro256** + splitmix64)\n";
    test::run("эталон splitmix64", &reference_splitmix64);
    test::run("эталон xoshiro256**", &reference_xoshiro);
    test::run("одно зерно — одна последовательность", &same_seed_same_sequence);
    test::run("разные зёрна — разные последовательности", &different_seeds_differ);
    test::run("зафиксированные значения для зерна 2026", &golden_values);
    test::run("независимые потоки игроков", &streams_are_independent);
    test::run("границы диапазонов", &ranges_and_bounds);
    test::run("нет смещения в малом диапазоне", &unbiased_small_range);
    test::run("монета с вероятностью", &coin);
    test::run("случайный элемент и перемешивание", &pick_and_shuffle);
    return test::finish();
}
