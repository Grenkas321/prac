#include "roles/commissar.hpp"

#include <algorithm>

namespace mafia {

namespace {

bool offered(const Request& request, NightActionKind action) {
    return std::ranges::find(request.actions, action) != request.actions.end();
}

}  // namespace

Response Commissar::act(const Request& request) {
    Response response;

    if (offered(request, NightActionKind::Shoot)) {
        for (PlayerId t : request.targets) {
            if (auto it = checked_.find(t); it != checked_.end() && it->second) {
                response.night.kind = NightActionKind::Shoot;
                response.night.target = t;
                response.speech = "Проверка подтвердила: " + name_of(request, t) + " — мафия. Стреляю.";
                return response;
            }
        }
    }

    if (offered(request, NightActionKind::Check)) {
        std::vector<PlayerId> unchecked;
        for (PlayerId t : request.targets) {
            if (!checked_.contains(t)) {
                unchecked.push_back(t);
            }
        }
        // Все уже проверены — проверяем повторно кого угодно: пропускать ход нельзя.
        const std::vector<PlayerId>& pool = unchecked.empty() ? request.targets : unchecked;
        if (auto target = choose_target(request, Purpose::Night, pool)) {
            response.night.kind = NightActionKind::Check;
            response.night.target = target;
        }
    }
    return response;
}

void Commissar::learn(const KnownIdentity& fact) {
    checked_[fact.id] = fact.is_mafia;  // повторная доставка того же факта безвредна
}

int Commissar::target_score(const Request& request, Purpose purpose, PlayerId candidate) const {
    if (purpose == Purpose::Night) {
        // Проверять выгоднее всего того, кого больше всех подозревают:
        // либо подтвердим мафию, либо снимем подозрение с честного.
        return score(beliefs().suspicion(candidate));
    }
    // Днём — сначала собственная память проверок (достоверно, даже если в этом
    // запросе сводки нет), затем общие оценки.
    if (auto it = checked_.find(candidate); it != checked_.end()) {
        return score(it->second ? Beliefs::kCertain : -Beliefs::kCertain);
    }
    return Civilian::target_score(request, purpose, candidate);
}

Player::Statement Commissar::statement(const Request& request) {
    // Намекает на проверенного мафиози, не раскрывая, откуда знает.
    for (PlayerId p : alive_others(request)) {
        if (auto it = checked_.find(p); it != checked_.end() && it->second) {
            Claim c;
            c.about = p;
            c.mafia = true;
            return Statement{"Я бы очень внимательно посмотрел на " + name_of(request, p) +
                                 ". Почти уверен — это мафия.",
                             c};
        }
    }
    return Civilian::statement(request);
}

Player::Statement Commissar::farewell(const Request& request) {
    // Завещание комиссара — единственное прощальное слово, которому правила
    // разрешают прямо указывать на мафию.
    for (bool mafia : {true, false}) {
        for (const auto& [id, is_mafia] : checked_) {
            if (is_mafia == mafia) {
                Claim c;
                c.about = id;
                c.mafia = mafia;
                return Statement{"Моё завещание: " + name_of(request, id) +
                                     (mafia ? " — мафия." : " — честный житель."),
                                 c};
            }
        }
    }
    return Civilian::farewell(request);
}

}  // namespace mafia
