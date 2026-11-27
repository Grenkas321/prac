#include "roles/extra_roles.hpp"

namespace mafia {

std::size_t Executioner::choose_option(const Request& request) {
    if (request.choice != ChoiceKind::Execute) {
        return Civilian::choose_option(request);
    }
    // Казнить лидера, которого подозреваю всерьёз; иначе — помиловать (последний вариант).
    std::size_t best = request.options.size() - 1;
    double top = 0.5;
    for (std::size_t i = 0; i < request.targets.size() && i + 1 < request.options.size(); ++i) {
        const double s = beliefs().suspicion(request.targets[i]);
        if (s > top) {
            top = s;
            best = i;
        }
    }
    return best;
}

std::size_t Necromancer::choose_option(const Request& request) {
    if (request.choice != ChoiceKind::Revive) {
        return Civilian::choose_option(request);
    }
    // Воскресить выбывшего, которому город доверял (вероятно, полезную роль);
    // мафию — никогда. Если достойных нет — подождать (последний вариант).
    std::size_t best = request.options.size() - 1;
    double top = 0.5;
    for (std::size_t i = 0; i < request.targets.size() && i + 1 < request.options.size(); ++i) {
        const PlayerId t = request.targets[i];
        if (beliefs().certain_mafia(t) == true) {
            continue;
        }
        const double v = town().trust(t) + town().pressure(t) * 0.3;
        if (v > top) {
            top = v;
            best = i;
        }
    }
    return best;
}

}  // namespace mafia
