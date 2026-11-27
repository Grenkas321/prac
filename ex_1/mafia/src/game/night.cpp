#include "game/night.hpp"

#include "game/voting.hpp"

#include <algorithm>
#include <set>

namespace mafia {

MafiaDecision decide_mafia(const std::vector<std::pair<PlayerId, PlayerId>>& votes, PlayerId boss) {
    std::vector<std::optional<PlayerId>> ballots;
    std::optional<PlayerId> boss_vote;
    for (const auto& [voter, target] : votes) {
        ballots.push_back(target);
        if (voter == boss) {
            boss_vote = target;
        }
    }
    const VoteTally tally = tally_votes(ballots);

    MafiaDecision d;
    if (tally.leaders.size() == 1) {
        d.victim = tally.leaders.front();
    } else if (!tally.leaders.empty()) {
        if (boss_vote && std::ranges::find(tally.leaders, *boss_vote) != tally.leaders.end()) {
            d.victim = boss_vote;
        } else {
            d.tied = tally.leaders;
        }
    }
    return d;
}

NightOutcome resolve_night(const std::vector<Attack>& attacks, std::optional<PlayerId> heal) {
    std::set<PlayerId> dead;
    std::set<PlayerId> saved;
    for (const Attack& a : attacks) {
        if (heal && a.target == *heal) {
            saved.insert(a.target);
        } else {
            dead.insert(a.target);
        }
    }
    NightOutcome out;
    out.deaths.assign(dead.begin(), dead.end());
    out.saved.assign(saved.begin(), saved.end());
    return out;
}

}  // namespace mafia
