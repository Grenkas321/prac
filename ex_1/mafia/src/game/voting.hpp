// Правила, которые не зависят от того, кто их исполняет: подсчёт голосов
// и проверка ответа игрока. Чистые функции — без состояния, без нитей,
// поэтому проверяются тестами напрямую, без запуска партии.
#pragma once

#include "game/protocol.hpp"
#include "game/types.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace mafia {

struct VoteTally {
    // (кандидат, голоса) — по убыванию голосов, при равенстве по номеру места.
    std::vector<std::pair<PlayerId, int>> counts;
    std::vector<PlayerId> leaders;  // все, у кого максимум голосов
    int top = 0;                    // максимум голосов
    int spoiled = 0;                // голоса, не засчитанные ведущим
};

// Бюллетень: номер кандидата или пусто (голос не засчитан).
VoteTally tally_votes(const std::vector<std::optional<PlayerId>>& ballots);

// То же с весами: weights[i] — вес бюллетеня i. Ведущий считает в «полуголосах»:
// живой — 2, выбывший мирный при некроманте — 1. Счёт в tally — в тех же единицах.
VoteTally tally_votes(const std::vector<std::optional<PlayerId>>& ballots, const std::vector<int>& weights);

// Что делать по итогам тура. Кандидаты — те, кто получил хотя бы один голос.
enum class VoteOutcome {
    Nobody,   // ни одного засчитанного голоса
    Execute,  // единственный лидер выбывает
    Revote,   // лидеров несколько, но не все кандидаты равны: переголосование между лидерами
    Poll,     // все кандидаты набрали поровну: опрос «выгнать всех разом?»
};

struct VoteDecision {
    VoteOutcome outcome = VoteOutcome::Nobody;
    std::vector<PlayerId> players;  // Execute — один; Revote и Poll — лидеры
};

VoteDecision decide_vote(const VoteTally& tally);

// Опрос «выгнать всех разом?» проходит, если «да» сказали больше половины голосовавших.
bool kick_all_passes(int yes, int voters);

// Ответ допустим, если он от адресата, на этот запрос и, для запросов
// с выбором, выбирает только из предложенного ведущим.
bool is_valid_answer(const Request& request, const Response& response);

}  // namespace mafia
