#include "roles/maniac.hpp"

#include <algorithm>

namespace mafia {

Response Maniac::act(const Request& request) {
    Response response;
    if (auto target = choose_target(request, Purpose::Night)) {
        response.night.kind = NightActionKind::Kill;
        response.night.target = target;
    }
    return response;
}

int Maniac::target_score(const Request& request, Purpose purpose, PlayerId candidate) const {
    if (purpose == Purpose::Night) {
        return score(beliefs().threat(candidate));  // обвинявшие меня и надёжные — первыми
    }
    return Player::target_score(request, purpose, candidate);
}

std::size_t Maniac::choose_option(const Request& request) {
    // «Нет», если в списке я сам; иначе «да»: чем меньше людей за столом, тем ближе победа.
    return std::ranges::any_of(request.targets, [this](PlayerId t) { return t == id(); }) ? 1 : 0;
}

}  // namespace mafia
