#include "roles/beliefs.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>
#include <variant>

namespace mafia {

namespace {

// ---------------------------------------------------------------------------
// Веса. Масштаб: 1.0 — «один голос обычного игрока против неизвестного».
// ---------------------------------------------------------------------------

// Действие против игрока, о котором ничего достоверного не известно.
// Голос весит больше слов: он влияет на казнь, а обвинение ничего не стоит.
constexpr double kVoteWeight = 1.0;
constexpr double kAccuseWeight = 0.6;
constexpr double kDefendWeight = 0.4;  // защита снижает подозрение

// Послужной список (когда цель действия раскрыта или достоверно известна).
constexpr double kRightCall = 1.0;          // обвинял/голосовал против мафии — попал
constexpr double kWrongCall = -0.6;         // против честного — промахнулся
constexpr double kWrongVoteSuspicion = 0.8; // голос против честного — подозрительно
constexpr double kWrongAccuseSuspicion = 0.5;
constexpr double kDefendedMafia = 2.0;      // защищал мафию — очень подозрительно
constexpr double kDefendedHonest = 0.3;     // защищал честного — плюс к доверию

// Переосмысление действий раскрытого мафиози: мафия своих не обвиняет
// и против своих не голосует, зато защищает их.
// «Скорее не мафия» — но не «честен»: не-мафия бывает и маньяком. Поэтому
// вес умеренный, а когда вся мафия раскрыта, он не действует вовсе.
constexpr double kMafiaAccusedHim = -0.4;   // кого мафиози обвинял — скорее не мафия
constexpr double kMafiaDefendedHim = 1.5;   // кого защищал — скорее союзник
constexpr double kVotedWithMafia = 0.4;     // голосовал заодно с мафиози в том же туре

// «Кому мешал убитый?» Честного убили ночью — подозрительны те, кого он
// обвинял и против кого голосовал: у них был повод его убрать. Единственный
// сигнал, который выдаёт маньяка (днём он ведёт себя как мирный).
constexpr double kNightVictimAccused = 1.2;

enum class ActKind { Accuse, Defend, Vote };

struct Act {
    PlayerId actor = 0;
    PlayerId about = 0;
    ActKind kind = ActKind::Accuse;
    int day = 0;
    int round = 0;
};

std::vector<Act> collect_acts(const PublicSnapshot& world) {
    std::vector<Act> acts;
    auto claim_act = [&](PlayerId speaker, const std::optional<Claim>& c, int day) {
        if (c && c->about != speaker) {
            acts.push_back(Act{speaker, c->about, c->mafia ? ActKind::Accuse : ActKind::Defend, day, 0});
        }
    };
    for (const HistoryEntry& h : world.history) {
        if (const auto* s = std::get_if<SpeechEvent>(&h.event)) {
            claim_act(s->speaker, s->claim, h.day);
        } else if (const auto* w = std::get_if<LastWordEvent>(&h.event)) {
            claim_act(w->speaker, w->claim, h.day);
        } else if (const auto* v = std::get_if<VoteEvent>(&h.event); v && v->target) {
            acts.push_back(Act{v->voter, *v->target, ActKind::Vote, h.day, v->round});
        }
    }
    return acts;
}

bool valid(PlayerId id, std::size_t n) {
    return id >= 0 && static_cast<std::size_t>(id) < n;
}

}  // namespace

Beliefs Beliefs::evaluate(const PublicSnapshot& world, PlayerId self, RoleKind role,
                          const std::vector<KnownIdentity>& known) {
    const std::size_t n = world.players.size();
    Beliefs b;
    b.opinions_.assign(n, Opinion{});
    b.certain_.assign(n, std::nullopt);

    // --- 1. Достоверное знание ---------------------------------------------
    for (const PublicPlayer& p : world.players) {
        if (p.disclosed) {
            // Объявил ведущий. Раскрытый убийца-одиночка (взлом хакера, открытое
            // объявление о выбывшем маньяке) — тоже враг мирных, живой или нет:
            // кто голосовал против него, тот «попал».
            const bool loner = p.disclosed->role && team_of(*p.disclosed->role) == Team::Loner;
            b.certain_[static_cast<std::size_t>(p.id)] = p.disclosed->is_mafia || loner;
        }
    }
    b.maybe_maniac_.assign(n, false);
    for (const KnownIdentity& k : known) {
        if (valid(k.id, n)) {
            b.certain_[static_cast<std::size_t>(k.id)] = k.is_mafia;  // союзники, проверки
            // Проверка «не мафия» не отличает мирного от маньяка: живой
            // проверенный остаётся под небольшим подозрением (см. suspicion()).
            // Если роль названа (священник представился, адвокат узнал) — сомнений нет.
            const bool role_known_honest = k.role && team_of(*k.role) != Team::Loner;
            if (!k.is_mafia && !role_known_honest && k.id != self &&
                !world.players[static_cast<std::size_t>(k.id)].disclosed) {
                b.maybe_maniac_[static_cast<std::size_t>(k.id)] = true;
            }
            // Названа роль убийцы-одиночки (свидетель, Каттани) — это враг мирных.
            if (k.role && team_of(*k.role) == Team::Loner) {
                b.certain_[static_cast<std::size_t>(k.id)] = true;
            }
        }
    }
    const bool i_am_mafia = team_of(role) == Team::Mafia;
    if (valid(self, n)) {
        b.certain_[static_cast<std::size_t>(self)] = i_am_mafia;  // себя знаю всегда
    }

    const std::vector<Act> acts = collect_acts(world);
    std::vector<double> track(n, 0.0);  // послужной список: сколько раз «попадал»

    // --- 2. Послужной список: действия, цель которых известна достоверно ----
    for (const Act& a : acts) {
        if (!valid(a.actor, n) || !valid(a.about, n)) {
            continue;
        }
        const auto target = b.certain_[static_cast<std::size_t>(a.about)];
        if (!target) {
            continue;
        }
        Opinion& actor = b.opinions_[static_cast<std::size_t>(a.actor)];
        double& t = track[static_cast<std::size_t>(a.actor)];
        switch (a.kind) {
            case ActKind::Accuse:
            case ActKind::Vote:
                if (*target) {
                    t += kRightCall;
                } else {
                    t += kWrongCall;
                    actor.suspicion += a.kind == ActKind::Vote ? kWrongVoteSuspicion : kWrongAccuseSuspicion;
                }
                break;
            case ActKind::Defend:
                if (*target) {
                    t -= kRightCall;
                    actor.suspicion += kDefendedMafia;
                } else {
                    t += kDefendedHonest;
                }
                break;
        }
    }

    // Доверие к словам: 1 — обычный игрок, больше — чаще попадал.
    std::vector<double> credibility(n, 1.0);
    for (std::size_t i = 0; i < n; ++i) {
        credibility[i] = std::clamp(1.0 + 0.5 * track[i], 0.2, 3.0);
        if (b.certain_[i] == false) {
            credibility[i] = std::max(credibility[i], 1.5);  // честный точно не врёт нарочно
        }
    }

    // Охота на маньяка: вся мафия публично раскрыта, а игра идёт — значит,
    // жив маньяк, и ночные смерти теперь только его рук дело.
    int revealed_mafia = 0;
    for (const PublicPlayer& p : world.players) {
        revealed_mafia += !p.alive && p.disclosed && p.disclosed->is_mafia ? 1 : 0;  // выбывшие (хакер раскрывает и живых)
    }
    const bool maniac_hunt = world.mafia_total > 0 && revealed_mafia >= world.mafia_total;

    // --- 3. Переосмысление действий раскрытой мафии -------------------------
    // Голоса, сгруппированные по (день, тур, цель): «кто голосовал вместе с ним».
    // Один проход вместо перебора всей истории на каждый голос мафии — иначе
    // при сотнях игроков пересчёт квадратичен по истории на каждый запрос.
    // Номера в группе идут в порядке истории — порядок сложений прежний.
    std::map<std::tuple<int, int, PlayerId>, std::vector<std::size_t>> same_vote;
    for (std::size_t i = 0; i < acts.size(); ++i) {
        const Act& v = acts[i];
        if (v.kind == ActKind::Vote && valid(v.actor, n)) {
            same_vote[{v.day, v.round, v.about}].push_back(i);
        }
    }
    for (const Act& a : acts) {
        if (!valid(a.actor, n) || !valid(a.about, n) || b.certain_[static_cast<std::size_t>(a.actor)] != true) {
            continue;
        }
        Opinion& about = b.opinions_[static_cast<std::size_t>(a.about)];
        switch (a.kind) {
            case ActKind::Accuse:
            case ActKind::Vote:
                if (!maniac_hunt) {  // в охоте на маньяка «не мафия» уже ничего не доказывает
                    about.suspicion += kMafiaAccusedHim;
                }
                break;
            case ActKind::Defend:
                about.suspicion += kMafiaDefendedHim;
                break;
        }
        if (a.kind == ActKind::Vote) {
            // a сам входит в свою группу: голос учтён в same_vote выше.
            for (std::size_t i : same_vote.at({a.day, a.round, a.about})) {  // голосовали вместе с ним
                const Act& other = acts[i];
                if (other.actor != a.actor) {
                    b.opinions_[static_cast<std::size_t>(other.actor)].suspicion += kVotedWithMafia;
                }
            }
        }
    }

    // --- 4. Подозрения к неизвестным: взвешенные доверием к говорящему -------
    for (const Act& a : acts) {
        if (!valid(a.actor, n) || !valid(a.about, n) || b.certain_[static_cast<std::size_t>(a.about)]) {
            continue;
        }
        if (b.certain_[static_cast<std::size_t>(a.actor)] == true) {
            continue;  // слова мафии уже учтены в обратную сторону (шаг 3)
        }
        const double c = credibility[static_cast<std::size_t>(a.actor)];
        Opinion& about = b.opinions_[static_cast<std::size_t>(a.about)];
        switch (a.kind) {
            case ActKind::Vote:
                about.suspicion += kVoteWeight * c;
                break;
            case ActKind::Accuse:
                about.suspicion += kAccuseWeight * c;
                break;
            case ActKind::Defend:
                about.suspicion -= kDefendWeight * c;
                break;
        }
    }

    // --- 4а. Кому мешал убитый ночью честный игрок -------------------------
    std::vector<bool> honest_night_victim(n, false);
    for (const HistoryEntry& h : world.history) {
        if (const auto* d = std::get_if<DeathEvent>(&h.event);
            d && d->cause == DeathCause::Night && !d->disclosed.is_mafia && valid(d->victim, n)) {
            honest_night_victim[static_cast<std::size_t>(d->victim)] = true;
        }
    }
    for (const Act& a : acts) {
        if (a.kind == ActKind::Defend || !valid(a.actor, n) || !valid(a.about, n) ||
            !honest_night_victim[static_cast<std::size_t>(a.actor)] ||
            b.certain_[static_cast<std::size_t>(a.about)]) {
            continue;
        }
        b.opinions_[static_cast<std::size_t>(a.about)].suspicion +=
            maniac_hunt ? 2.0 * kNightVictimAccused : kNightVictimAccused;
    }

    // --- 5. Доверие, давление и опасность для моей команды ------------------
    for (std::size_t i = 0; i < n; ++i) {
        b.opinions_[i].trust = credibility[i] - 1.0;
    }
    for (const Act& a : acts) {
        if (a.kind == ActKind::Defend || !valid(a.actor, n) || !valid(a.about, n)) {
            continue;
        }
        // Давит тот, кто бьёт по подозреваемым (а сильнее всего — по известной мафии).
        const auto c = b.certain_[static_cast<std::size_t>(a.about)];
        const double weight = c ? (*c ? 1.0 : 0.0) : std::clamp(b.opinions_[static_cast<std::size_t>(a.about)].suspicion / 3.0, 0.0, 1.0);
        b.opinions_[static_cast<std::size_t>(a.actor)].pressure += weight;
    }
    for (std::size_t i = 0; i < n; ++i) {
        Opinion& o = b.opinions_[i];
        if (i_am_mafia || role == RoleKind::Maniac) {
            // Опасен тот, кто верно бьёт по моей команде, и тот, кому верят.
            o.threat = 0.5 * std::max(0.0, o.trust);
        } else {
            o.threat = o.suspicion;  // мирным опасна мафия
        }
    }
    if (i_am_mafia || role == RoleKind::Maniac) {
        for (const Act& a : acts) {
            if (a.kind == ActKind::Defend || !valid(a.actor, n) || !valid(a.about, n)) {
                continue;
            }
            const bool hits_my_team = i_am_mafia ? b.certain_[static_cast<std::size_t>(a.about)] == true
                                                 : a.about == self;
            if (hits_my_team) {
                b.opinions_[static_cast<std::size_t>(a.actor)].threat += a.kind == ActKind::Vote ? 1.0 : 0.7;
            }
        }
    }
    return b;
}

std::optional<bool> Beliefs::certain_mafia(PlayerId id) const {
    if (!valid(id, certain_.size())) {
        return std::nullopt;
    }
    return certain_[static_cast<std::size_t>(id)];
}

const Opinion& Beliefs::opinion(PlayerId id) const {
    static const Opinion none;
    return valid(id, opinions_.size()) ? opinions_[static_cast<std::size_t>(id)] : none;
}

double Beliefs::suspicion(PlayerId id) const {
    if (valid(id, maybe_maniac_.size()) && maybe_maniac_[static_cast<std::size_t>(id)]) {
        // «Точно не мафия», но может быть маньяком: улики против него весят
        // втрое меньше и со сдвигом к невиновности — но не обнуляются.
        return 0.3 * opinion(id).suspicion - 1.0;
    }
    if (auto c = certain_mafia(id)) {
        return *c ? kCertain : -kCertain;
    }
    return opinion(id).suspicion;
}

double Beliefs::trust(PlayerId id) const {
    return opinion(id).trust;
}

double Beliefs::threat(PlayerId id) const {
    return opinion(id).threat;
}

double Beliefs::pressure(PlayerId id) const {
    return opinion(id).pressure;
}

}  // namespace mafia
