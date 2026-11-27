#include "roles/doctor.hpp"

#include <algorithm>

namespace mafia {

Response Doctor::act(const Request& request) {
    std::vector<PlayerId> allowed;
    for (PlayerId t : request.targets) {
        if (t == last_healed_) {
            continue;  // не две ночи подряд
        }
        if (t == id() && healed_self_) {
            continue;  // себя — только однажды
        }
        allowed.push_back(t);
    }
    // Ведущий — последняя инстанция: если по нашей памяти выбирать не из чего,
    // доверяем его списку (пропускать ход доктору нельзя).
    const std::vector<PlayerId>& pool = allowed.empty() ? request.targets : allowed;

    Response response;
    if (auto target = choose_target(request, Purpose::Night, pool)) {
        response.night.kind = NightActionKind::Heal;
        response.night.target = target;
        last_healed_ = target;
        healed_self_ = healed_self_ || *target == id();
    }
    return response;
}

int Doctor::target_score(const Request& request, Purpose purpose, PlayerId candidate) const {
    if (purpose == Purpose::Night) {
        // Лечим того, кого мафия захочет убрать. Мафия видит стол как город —
        // поэтому давление и доверие берём ИЗ ПУБЛИЧНОГО взгляда town(): своё
        // тайное знание («я-то точно не мафия») тут только мешает предсказать
        // выбор убийцы. Подозрительного (по своим оценкам) не лечим. Себя — без
        // поблажек: право лечить себя одно на игру, тратить его зря невыгодно.
        // Только положительная часть подозрения: «точно не мафия» (−100, в том
        // числе я сам) не должно превращаться в огромный плюс к лечению.
        return score(town().pressure(candidate) + town().trust(candidate) -
                     0.5 * std::max(0.0, beliefs().suspicion(candidate)));
    }
    return Civilian::target_score(request, purpose, candidate);
}

bool Doctor::may_protect(const GameState& state, PlayerId self, PlayerId target) {
    const int tonight = state.day;  // ночь N идёт после дня N
    for (const NightRecord& r : state.night_log) {
        if (r.actor != self || r.action.kind != NightActionKind::Heal || r.action.target != target) {
            continue;
        }
        if (r.night == tonight - 1) {
            return false;  // лечил эту цель прошлой ночью
        }
        if (target == self && r.night < tonight) {
            return false;  // себя уже лечил
        }
    }
    return true;
}

}  // namespace mafia
