#include "roles/player.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <string_view>

namespace mafia {

namespace {

// Банк реплик. {} — имя игрока; имя всегда в именительном падеже.
constexpr std::string_view kAccusations[] = {
    "{} ведёт себя слишком спокойно для мирного.",
    "Мне кажется, {} что-то скрывает.",
    "{} слишком старательно отводит от себя подозрения.",
    "У меня вопросы к тому, как голосует {}.",
    "{} — первый в моём списке подозреваемых.",
    "Если честно, {} мне не нравится.",
    "Предлагаю присмотреться: {} говорит много, а по делу — ничего.",
    "Посмотрите, против кого голосовал {}. Это не случайно.",
    "{} слишком уверенно обвиняет других. Так ведёт себя мафия.",
    "Я голосую сегодня против: {}. Объясню — слишком много совпадений.",
    "{} путается в своих словах. Это мафия, уверен.",
    "Вчера {} защищал не того. Делайте выводы.",
};

constexpr std::string_view kDefences[] = {
    "{} — точно не мафия, я за него ручаюсь.",
    "Мне кажется, {} честный игрок. Не трогайте его.",
    "{} голосует разумно. Я ему верю.",
    "Не надо на {} давить — он на нашей стороне.",
};

constexpr std::string_view kNeutral[] = {
    "Пока сложно что-то сказать, давайте послушаем остальных.",
    "Давайте не торопиться с голосованием.",
    "Предлагаю внимательнее смотреть, кто за кого голосует.",
    "Меня пока никто не убедил.",
    "Кто-то из нас врёт, и это видно по голосам.",
    "Слишком мало информации. Я подожду, что скажут другие.",
    "Обратите внимание, кто молчит, а кто слишком громко обвиняет.",
};

constexpr std::string_view kFarewell[] = {
    "Что ж, удачи оставшимся. Будьте внимательнее.",
    "Вы ошиблись, но игра ещё не окончена.",
    "Ухожу с чистой совестью.",
    "Надеюсь, вы найдёте настоящих виновников.",
};

// Порог уверенного обвинения: примерно «двое против него проголосовали».
constexpr double kAccuseThreshold = 1.0;
// Без явных подозрений бот иногда обвиняет «по интуиции» — так в начале
// партии появляются первые сигналы, из которых потом складываются оценки.
constexpr double kHunchChance = 0.35;

std::string fill(std::string_view pattern, const std::string& name) {
    std::string out(pattern);
    if (auto pos = out.find("{}"); pos != std::string::npos) {
        out.replace(pos, 2, name);
    }
    return out;
}

template <std::size_t N>
std::string_view pick_line(Rng& rng, const std::string_view (&lines)[N]) {
    return lines[rng.below(N)];
}

}  // namespace

Player::Player(PlayerSetup setup, RoleKind role)
    : id_(setup.id), name_(std::move(setup.name)), role_(role), rng_(setup.rng) {}

Response Player::handle(const Request& request) {
    absorb(request.you);
    if (request.world) {
        // Оценки — с нуля по всей публичной истории и своему знанию.
        // Роль — текущая из сводки: она могла смениться (сержант -> комиссар).
        beliefs_ = Beliefs::evaluate(*request.world, id_, request.you.role, request.you.known);
        town_ = Beliefs::evaluate(*request.world, -1, RoleKind::Civilian, {});
    }

    Response response;
    switch (request.kind) {
        case RequestKind::NightMove:
            // Есть свой ночной ход (подключён фабрикой) — им. Нет (мирный,
            // получивший от донора право проверки) — общей политикой действий.
            response = night_move_ ? night_move_(request) : act_by_policy(request);
            break;
        case RequestKind::Speech:
            response = speak(request);
            break;
        case RequestKind::Vote:
            response = vote(request);
            break;
        case RequestKind::LastWord:
            response = last_word(request);
            break;
        case RequestKind::Choice:
            response = choose(request);
            break;
        case RequestKind::Stop:
            break;
    }
    // Протокольные поля — в одном месте, для любой роли.
    response.request_id = request.id;
    response.from = id_;
    return response;
}

Response Player::vote(const Request& request) {
    Response response;
    if (std::optional<AdvisorAnswer> a = consult(request);
        a && a->target && std::ranges::find(request.targets, *a->target) != request.targets.end()) {
        response.target = a->target;  // совет годится — цель из списка ведущего
        response.speech = a->speech;
        return response;
    }
    response.target = choose_target(request, Purpose::Vote);
    if (response.target) {
        response.speech = "Голосую против: " + name_of(request, *response.target) + ".";
    }
    return response;
}

Response Player::speak(const Request& request) {
    if (std::optional<AdvisorAnswer> a = consult(request); a && !a->speech.empty()) {
        Response response;
        response.speech = a->speech;
        response.claim = a->claim;
        return response;
    }
    Statement s = statement(request);
    Response response;
    response.speech = std::move(s.text);
    response.claim = s.claim;
    return response;
}

Response Player::last_word(const Request& request) {
    if (std::optional<AdvisorAnswer> a = consult(request); a && !a->speech.empty()) {
        Response response;
        response.speech = a->speech;
        return response;
    }
    Statement s = farewell(request);
    Response response;
    response.speech = std::move(s.text);
    response.claim = s.claim;
    return response;
}

std::optional<AdvisorAnswer> Player::consult(const Request& request) noexcept {
    if (!advisor_) {
        return std::nullopt;
    }
    try {
        return advisor_->advise(request);
    } catch (...) {
        return std::nullopt;  // любой сбой советника — решаем сами, партия не падает
    }
}

Response Player::choose(const Request& request) {
    Response response;
    if (!request.options.empty()) {
        response.choice = choose_option(request);
    }
    return response;
}

std::optional<PlayerId> Player::choose_target(const Request& request, Purpose purpose) {
    return choose_target(request, purpose, request.targets);
}

std::optional<PlayerId> Player::choose_target(const Request& request, Purpose purpose,
                                              const std::vector<PlayerId>& candidates) {
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::vector<PlayerId> best;
    int best_score = 0;
    for (PlayerId c : candidates) {
        int score = target_score(request, purpose, c);
        if (best.empty() || score > best_score) {
            best = {c};
            best_score = score;
        } else if (score == best_score) {
            best.push_back(c);
        }
    }
    return rng_.pick(best);
}

// ---------------------------------------------------------------------------
// Общая политика ночных действий: по виду действия и своим оценкам.
// Ею пользуются дополнительные роли (у них нет особой логики выбора),
// а также любой игрок, которому ведущий предложил непривычное действие.
// ---------------------------------------------------------------------------

std::optional<PlayerId> Player::best_target(const std::vector<PlayerId>& candidates,
                                            const std::function<double(PlayerId)>& value) {
    std::vector<PlayerId> best;
    double top = 0;
    for (PlayerId c : candidates) {
        const double v = value(c);
        if (best.empty() || v > top + 1e-9) {
            best = {c};
            top = v;
        } else if (std::abs(v - top) <= 1e-9) {
            best.push_back(c);
        }
    }
    if (best.empty()) {
        return std::nullopt;
    }
    return rng_.pick(best);  // равные — случайно, из своего потока
}

Response Player::act_by_policy(const Request& request) {
    Response response;
    if (request.actions.empty() || request.targets.empty()) {
        return response;  // хода нет
    }
    const bool mafia = team_of(request.you.role) == Team::Mafia;
    const auto has = [&](NightActionKind k) {
        return std::ranges::find(request.actions, k) != request.actions.end();
    };
    // Для мафии своих не существует как целей; для остальных — себя.
    const auto ours = [&](PlayerId c) { return mafia ? beliefs_.certain_mafia(c) == true : c == id_; };
    const auto enemy = [&](PlayerId c) { return ours(c) ? -1e4 : beliefs_.suspicion(c); };
    const auto victim_like = [&](PlayerId c) {  // кого, вероятно, захотят убрать: давит и ему верят
        return ours(c) ? -1e4 : town_.pressure(c) + town_.trust(c);
    };
    const auto unknown_enemy = [&](PlayerId c) {  // проверять выгоднее непроверенных
        return beliefs_.certain_mafia(c) ? -50.0 : enemy(c);
    };

    // Какое действие: выстрел — только по достоверному врагу (или если других нет).
    NightActionKind kind = NightActionKind::None;
    const bool sure_enemy = std::ranges::any_of(request.targets, [&](PlayerId c) {
        return !mafia && beliefs_.certain_mafia(c) == true;
    });
    if (has(NightActionKind::Shoot) && (sure_enemy || (mafia && !has(NightActionKind::Check)))) {
        kind = NightActionKind::Shoot;
    } else if (has(NightActionKind::Check)) {
        kind = NightActionKind::Check;
    } else {
        for (NightActionKind k : request.actions) {
            if (k != NightActionKind::None && k != NightActionKind::Antidote) {
                kind = k;
                break;
            }
        }
    }

    std::function<double(PlayerId)> value;
    switch (kind) {
        case NightActionKind::Kill:
        case NightActionKind::Shoot:
        case NightActionKind::Poison:
            value = mafia || team_of(request.you.role) == Team::Loner
                        ? std::function<double(PlayerId)>([&](PlayerId c) { return ours(c) ? -1e4 : beliefs_.threat(c) + 0.1 * victim_like(c); })
                        : std::function<double(PlayerId)>(enemy);
            break;
        case NightActionKind::Check:
            value = mafia ? std::function<double(PlayerId)>(victim_like) : std::function<double(PlayerId)>(unknown_enemy);
            break;
        case NightActionKind::Heal:
        case NightActionKind::Guard:
        case NightActionKind::Listen:
        case NightActionKind::Watch:
            value = [&](PlayerId c) { return victim_like(c) - 0.5 * std::max(0.0, beliefs_.suspicion(c)); };
            break;
        case NightActionKind::Visit:
        case NightActionKind::Hack:
        case NightActionKind::Compare:
            value = unknown_enemy;
            break;
        case NightActionKind::Block:
        case NightActionKind::Slander:
        case NightActionKind::Seek:
        case NightActionKind::Steal:
        case NightActionKind::Admire:
        case NightActionKind::Kidnap:
            value = victim_like;
            break;
        case NightActionKind::Freeze:
            value = [&](PlayerId c) { return ours(c) ? -1e4 : beliefs_.threat(c); };
            break;
        case NightActionKind::Revive:
            value = [&](PlayerId c) { return town_.trust(c) - (beliefs_.certain_mafia(c) == true ? 100.0 : 0.0); };
            break;
        case NightActionKind::Prank:
            value = [](PlayerId) { return 0.0; };
            break;
        case NightActionKind::None:
        case NightActionKind::Antidote:
            return response;
    }

    const std::optional<PlayerId> target = best_target(request.targets, value);
    if (!target) {
        return response;
    }
    // Необязательные ходы: без явного повода лучше промолчать.
    if (request.can_abstain) {
        const double v = value(*target);
        const bool weak = (kind == NightActionKind::Kill && v < 1.5) ||
                          (kind == NightActionKind::Visit && v < 0.5 && rng_.chance(0.5)) ||
                          (kind == NightActionKind::Kidnap && rng_.chance(0.75)) ||
                          (kind == NightActionKind::Steal && rng_.chance(0.3)) ||
                          (kind == NightActionKind::Revive && v < 0.0);
        if (weak) {
            return response;
        }
    }
    response.night.kind = kind;
    response.night.target = target;
    if (kind == NightActionKind::Compare) {
        std::vector<PlayerId> rest;
        std::ranges::copy_if(request.targets, std::back_inserter(rest), [&](PlayerId c) { return c != *target; });
        response.night.second = best_target(rest, [&](PlayerId c) { return town_.trust(c); });
        if (!response.night.second) {
            response.night = NightAction{};
        }
    }
    if (kind == NightActionKind::Kidnap) {
        response.night.guess = RoleKind::Commissar;  // тот, кто сильнее всех давит, — вероятно комиссар
    }
    return response;
}

int Player::score(double value) {
    return static_cast<int>(std::lround(std::clamp(value, -1e5, 1e5) * 100.0));
}

int Player::target_score(const Request&, Purpose purpose, PlayerId candidate) const {
    if (purpose == Purpose::Vote) {
        return score(beliefs_.suspicion(candidate));
    }
    return 0;
}

std::size_t Player::choose_option(const Request& request) {
    if (request.choice != ChoiceKind::KickAll) {
        return request.options.size() - 1;  // палач / некромант — не наша роль: последний вариант «нет»
    }
    if (request.targets.empty()) {
        return static_cast<std::size_t>(rng_.below(request.options.size()));
    }
    if (std::ranges::any_of(request.targets, [this](PlayerId t) { return t == id_; })) {
        return 1;  // «выгнать всех», когда в списке я сам, — нет
    }
    double total = 0;
    for (PlayerId t : request.targets) {
        total += beliefs_.suspicion(t);
    }
    return total / static_cast<double>(request.targets.size()) >= 0.5 ? 0 : 1;
}

Player::Statement Player::statement(const Request& request) {
    const std::vector<PlayerId> others = alive_others(request);
    std::optional<PlayerId> top;
    double best = 0;
    for (PlayerId p : others) {
        const double s = beliefs_.suspicion(p);
        if (!top || s > best) {
            top = p;
            best = s;
        }
    }
    if (top && best >= kAccuseThreshold) {
        return accuse(request, *top);
    }
    for (PlayerId p : others) {  // знаю честного — могу за него заступиться
        if (beliefs_.certain_mafia(p) == false && rng_.chance(0.3)) {
            return defend(request, p);
        }
    }
    if (top && rng_.chance(kHunchChance)) {
        // Слабое подозрение или «интуиция»: обвиняем самого подозрительного,
        // а если все равны — того, кто попался.
        return accuse(request, best > 0 ? *top : rng_.pick(others));
    }
    return neutral();
}

Player::Statement Player::farewell(const Request&) {
    return Statement{std::string(pick_line(rng_, kFarewell)), std::nullopt};
}

std::vector<PlayerId> Player::alive_others(const Request& request) const {
    std::vector<PlayerId> out;
    if (!request.world) {
        return out;
    }
    for (const PublicPlayer& p : request.world->players) {
        if (p.alive && p.id != id_) {
            out.push_back(p.id);
        }
    }
    return out;
}

std::string Player::name_of(const Request& request, PlayerId id) const {
    if (request.world && id >= 0 && static_cast<std::size_t>(id) < request.world->players.size()) {
        return request.world->players[static_cast<std::size_t>(id)].name;
    }
    return "Игрок " + std::to_string(id + 1);
}

Player::Statement Player::accuse(const Request& request, PlayerId suspect) {
    Claim c;
    c.about = suspect;
    c.mafia = true;
    return Statement{fill(pick_line(rng_, kAccusations), name_of(request, suspect)), c};
}

Player::Statement Player::defend(const Request& request, PlayerId honest) {
    Claim c;
    c.about = honest;
    c.mafia = false;
    return Statement{fill(pick_line(rng_, kDefences), name_of(request, honest)), c};
}

Player::Statement Player::neutral() {
    return Statement{std::string(pick_line(rng_, kNeutral)), std::nullopt};
}

void Player::absorb(const Briefing& briefing) {
    if (!secret_sink_) {
        return;
    }
    for (const KnownIdentity& fact : briefing.known) {
        if (fact.id != id_) {
            secret_sink_(fact);
        }
    }
}

}  // namespace mafia
