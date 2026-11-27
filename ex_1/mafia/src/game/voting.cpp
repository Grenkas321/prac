#include "game/voting.hpp"

#include <algorithm>
#include <map>
#include <iterator>
#include <ranges>

namespace mafia {

VoteTally tally_votes(const std::vector<std::optional<PlayerId>>& ballots) {
    return tally_votes(ballots, std::vector<int>(ballots.size(), 1));  // у всех вес 1
}

VoteTally tally_votes(const std::vector<std::optional<PlayerId>>& ballots, const std::vector<int>& weights) {
    VoteTally tally;
    // Бюллетень и его вес идут парой: zip-представления в C++20 нет, поэтому
    // индексы; засчитанные — с непустым optional.
    std::map<PlayerId, int> votes;
    for (std::size_t i = 0; i < ballots.size(); ++i) {
        if (ballots[i]) {
            votes[*ballots[i]] += i < weights.size() ? weights[i] : 1;
        }
    }
    // Пустой optional, а не std::nullopt: алгоритму нужен значение того же типа,
    // что и элементы (nullopt_t с optional не связаны концептом сравнимости).
    tally.spoiled = static_cast<int>(std::ranges::count(ballots, std::optional<PlayerId>{}));

    // Сортировка кандидатов: по убыванию голосов, при равенстве — по номеру
    // (map уже упорядочен по номеру, stable_sort этот порядок сохраняет).
    // Проекция &pair::second — сравниваем голоса, не описывая лямбду-компаратор.
    tally.counts.assign(votes.begin(), votes.end());
    std::ranges::stable_sort(tally.counts, std::ranges::greater{}, &std::pair<PlayerId, int>::second);

    if (!tally.counts.empty()) {
        tally.top = tally.counts.front().second;
        // Лидеры — префикс отсортированного списка: take_while останавливается
        // на первом кандидате с меньшим числом голосов, дальше не смотрит.
        auto leaders = tally.counts |
                       std::views::take_while([top = tally.top](const auto& c) { return c.second == top; }) |
                       std::views::keys;
        // take_while — не «обычный» диапазон: его конец — страж другого типа,
        // поэтому не assign(begin, end), а ranges::copy.
        std::ranges::copy(leaders, std::back_inserter(tally.leaders));
    }
    return tally;
}

VoteDecision decide_vote(const VoteTally& tally) {
    VoteDecision d;
    d.players = tally.leaders;
    if (tally.leaders.empty()) {
        d.outcome = VoteOutcome::Nobody;
    } else if (tally.leaders.size() == 1) {
        d.outcome = VoteOutcome::Execute;
    } else if (tally.leaders.size() == tally.counts.size()) {
        d.outcome = VoteOutcome::Poll;  // равны все, кто получил голоса (3-3, 4-4-4)
    } else {
        // Лидеров меньше, чем кандидатов (3-3-1, 5-5-5-3): в следующем туре
        // кандидатов строго меньше, поэтому переголосования не зациклятся.
        d.outcome = VoteOutcome::Revote;
    }
    return d;
}

bool kick_all_passes(int yes, int voters) {
    return 2 * yes > voters;
}

namespace {

bool contains(const std::vector<PlayerId>& v, PlayerId x) {
    return std::ranges::find(v, x) != v.end();  // «есть ли хоть один такой» — без пары итераторов
}

}  // namespace

bool is_valid_answer(const Request& request, const Response& response) {
    if (response.from != request.you.self || response.request_id != request.id) {
        return false;  // чужой или устаревший ответ
    }
    switch (request.kind) {
        case RequestKind::Vote:
            if (!response.target) {
                return request.can_abstain;
            }
            // Список целей собрал ведущий: в нём только живые, без самого
            // голосующего. Значит, голос за мёртвого, за себя или за
            // несуществующего номера отсекается одной проверкой.
            return contains(request.targets, *response.target);
        case RequestKind::Choice:
            return response.choice && *response.choice < request.options.size();
        case RequestKind::NightMove:
            if (response.night.kind == NightActionKind::None) {
                return request.can_abstain;
            }
            return std::ranges::find(request.actions, response.night.kind) != request.actions.end() &&
                   response.night.target && contains(request.targets, *response.night.target);
        case RequestKind::Speech:
        case RequestKind::LastWord:
        case RequestKind::Stop:
            return true;
    }
    return false;
}

}  // namespace mafia
