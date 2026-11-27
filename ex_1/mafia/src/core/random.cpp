#include "core/random.hpp"

#include <limits>

namespace mafia {

namespace {

constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
}

}  // namespace

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

Rng::Rng(std::uint64_t seed) noexcept {
    for (std::uint64_t& word : s_) {
        word = splitmix64(seed);
    }
}

Rng Rng::stream(std::uint64_t seed, std::uint64_t index) noexcept {
    Rng rng(seed);
    for (std::uint64_t i = 0; i < index; ++i) {
        rng.jump();
    }
    return rng;
}

Rng Rng::from_state(const State& state) {
    if (state[0] == 0 && state[1] == 0 && state[2] == 0 && state[3] == 0) {
        throw std::invalid_argument("Rng::from_state: нулевое состояние");
    }
    Rng rng;
    rng.s_ = state;
    return rng;
}

// Эталонный xoshiro256**: выход — из s[1], затем линейное обновление состояния.
std::uint64_t Rng::next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

// Без смещения. Наивное next() % n чаще даёт малые значения, если 2^64
// не делится на n. Отбрасываем «хвост» — первые (2^64 mod n) значений,
// тогда оставшийся диапазон делится на n нацело. (0 - n) % n — это и есть
// 2^64 mod n в беззнаковой арифметике. Цикл почти всегда делает один шаг.
std::uint64_t Rng::below(std::uint64_t n) {
    if (n == 0) {
        throw std::invalid_argument("Rng::below: n должно быть > 0");
    }
    const std::uint64_t threshold = (0 - n) % n;
    for (;;) {
        const std::uint64_t x = next();
        if (x >= threshold) {
            return x % n;
        }
    }
}

std::int64_t Rng::uniform_int(std::int64_t lo, std::int64_t hi) {
    if (lo > hi) {
        throw std::invalid_argument("Rng::uniform_int: lo > hi");
    }
    // Ширину считаем в беззнаковых: hi - lo в знаковых может переполниться.
    const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    const std::uint64_t offset =
        span == std::numeric_limits<std::uint64_t>::max() ? next() : below(span + 1);
    // Сложение по модулю 2^64 и обратное приведение определены в C++20.
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + offset);
}

// Старшие 53 бита — ровно мантисса double; умножение на 2^-53 точное.
double Rng::uniform_real() noexcept {
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
}

double Rng::uniform_real(double lo, double hi) noexcept {
    return lo + (hi - lo) * uniform_real();
}

bool Rng::chance(double p) noexcept {
    if (p <= 0.0) {
        return false;
    }
    if (p >= 1.0) {
        return true;
    }
    return uniform_real() < p;
}

// Многочлен прыжка на 2^128 из эталонной реализации xoshiro256**.
void Rng::jump() noexcept {
    static constexpr std::uint64_t kJump[] = {0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
                                              0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL};
    State acc{};
    for (std::uint64_t word : kJump) {
        for (int bit = 0; bit < 64; ++bit) {
            if (word & (std::uint64_t{1} << bit)) {
                for (std::size_t i = 0; i < acc.size(); ++i) {
                    acc[i] ^= s_[i];
                }
            }
            next();
        }
    }
    s_ = acc;
}

}  // namespace mafia
