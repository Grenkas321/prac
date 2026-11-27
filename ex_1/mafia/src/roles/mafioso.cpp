#include "roles/mafioso.hpp"

#include <algorithm>
#include <variant>

namespace mafia {

namespace {

constexpr double kNever = -1e4;  // своих не выбираем никогда

}  // namespace

Response Mafioso::act(const Request& request) {
    Response response;
    if (auto target = choose_target(request, Purpose::Night)) {
        response.night.kind = NightActionKind::Kill;
        response.night.target = target;
        // Реплика в приват мафии: голос за жертву сопровождается предложением.
        response.speech = "Предлагаю убрать: " + name_of(request, *target) + ".";
    }
    return response;
}

void Mafioso::learn(const KnownIdentity& fact) {
    if (fact.is_mafia) {
        allies_.insert(fact.id);
    }
}

bool Mafioso::exposes_ally(const Request& request, PlayerId candidate) const {
    if (!request.world) {
        return false;
    }
    std::optional<PlayerId> last;
    for (const HistoryEntry& h : request.world->history) {
        if (const auto* s = std::get_if<SpeechEvent>(&h.event); s && s->speaker == candidate && s->claim &&
                                                                 s->claim->mafia) {
            last = s->claim->about;
        } else if (const auto* v = std::get_if<VoteEvent>(&h.event); v && v->voter == candidate && v->target) {
            last = v->target;
        }
    }
    return last && (ally(*last) || *last == id());
}

int Mafioso::target_score(const Request& request, Purpose purpose, PlayerId candidate) const {
    if (!in_mafia(request)) {
        return Player::target_score(request, purpose, candidate);  // подкидыш до превращения
    }
    if (ally(candidate) || candidate == id()) {
        return score(kNever);
    }
    if (purpose == Purpose::Vote) {
        // Голосуем «как город»: против того, кого и так подозревают, — так
        // голос мафиози не выделяется. Плюс — если это ещё и опасный игрок.
        return score(town().suspicion(candidate) + 0.5 * beliefs().threat(candidate));
    }
    // Ночью — самый опасный для мафии, но не тот, чья смерть укажет на союзника.
    return score(beliefs().threat(candidate) - (exposes_ally(request, candidate) ? 1.0 : 0.0));
}

std::size_t Mafioso::choose_option(const Request& request) {
    if (!in_mafia(request) || request.choice != ChoiceKind::KickAll) {
        return Player::choose_option(request);
    }
    const bool ours = std::ranges::any_of(request.targets, [this](PlayerId t) { return t == id() || ally(t); });
    return ours ? 1 : 0;  // своих не выгоняем; выгнать разом нескольких мирных — мафии на руку
}

Player::Statement Mafioso::statement(const Request& request) {
    if (!in_mafia(request)) {
        return Player::statement(request);
    }
    // Подставляем: обвиняем того, кого город и так подозревает (или опасного).
    std::optional<PlayerId> scapegoat;
    double best = 0;
    for (PlayerId p : alive_others(request)) {
        if (ally(p)) {
            continue;  // своих — никогда
        }
        const double s = town().suspicion(p) + 0.5 * beliefs().threat(p);
        if (!scapegoat || s > best) {
            scapegoat = p;
            best = s;
        }
    }
    if (scapegoat && rng().chance(0.7)) {
        return accuse(request, *scapegoat);
    }
    Claim innocent;  // «я мирный» — ложь, но и все мирные говорят так же
    innocent.about = id();
    innocent.mafia = false;
    return Statement{"Я мирный, мне скрывать нечего.", innocent};
}

}  // namespace mafia
