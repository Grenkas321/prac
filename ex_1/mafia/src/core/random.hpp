// Rng — собственный генератор псевдослучайных чисел с явным зерном.
//
// Алгоритм: xoshiro256** (Блэкман, Винья), состояние — 4 × 64 бита.
// Зерно разворачивается в состояние через splitmix64.
// Все методы распределений написаны здесь же, только на целочисленной
// арифметике, поэтому одно зерно даёт одну и ту же последовательность
// на любом компиляторе, стандартной библиотеке и платформе.
//
// Независимые потоки: Rng::stream(seed, k) — генератор зерна seed,
// продвинутый на k * 2^128 шагов (jump). Потоки с разными k не пересекаются.
// Договорённость: поток 0 — ведущий, поток i + 1 — игрок с PlayerId i.
//
// Не потокобезопасен намеренно: у каждой нити свой экземпляр.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mafia {

// Один шаг splitmix64: продвигает state и возвращает перемешанное значение.
std::uint64_t splitmix64(std::uint64_t& state) noexcept;

class Rng {
public:
    using State = std::array<std::uint64_t, 4>;

    // Состояние — четыре числа splitmix64 от зерна. Любое зерно, включая 0,
    // даёт хорошо перемешанное ненулевое состояние.
    explicit Rng(std::uint64_t seed) noexcept;

    // k-й независимый поток от общего зерна.
    static Rng stream(std::uint64_t seed, std::uint64_t index) noexcept;

    // Прямая установка состояния — для сверки с эталонными значениями алгоритма.
    // Состояние из одних нулей недопустимо (генератор выдавал бы только нули).
    static Rng from_state(const State& state);

    // Следующие 64 случайных бита.
    std::uint64_t next() noexcept;

    // Целое в [0, n) без смещения; n > 0.
    std::uint64_t below(std::uint64_t n);

    // Целое в [lo, hi] — обе границы включены; lo <= hi.
    std::int64_t uniform_int(std::int64_t lo, std::int64_t hi);

    // Вещественное в [0, 1): 53 случайных бита мантиссы.
    double uniform_real() noexcept;

    // Вещественное в [lo, hi).
    double uniform_real(double lo, double hi) noexcept;

    // Монета: true с вероятностью p (p <= 0 — никогда, p >= 1 — всегда).
    bool chance(double p) noexcept;

    // Случайный элемент непустого вектора.
    template <class T>
    const T& pick(const std::vector<T>& items) {
        if (items.empty()) {
            throw std::invalid_argument("Rng::pick: пустой вектор");
        }
        return items[static_cast<std::size_t>(below(items.size()))];
    }

    // Перемешивание Фишера — Йетса. Свой вариант, а не std::shuffle:
    // алгоритм std::shuffle стандартом не зафиксирован.
    template <class T>
    void shuffle(std::vector<T>& items) {
        for (std::size_t i = items.size(); i > 1; --i) {
            std::size_t j = static_cast<std::size_t>(below(i));  // j в [0, i)
            using std::swap;
            swap(items[i - 1], items[j]);
        }
    }

    // Продвинуть генератор на 2^128 шагов (эквивалентно 2^128 вызовам next()).
    void jump() noexcept;

    const State& state() const noexcept { return s_; }

private:
    Rng() = default;

    State s_{};
};

}  // namespace mafia
