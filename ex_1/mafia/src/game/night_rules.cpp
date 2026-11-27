#include "game/night_rules.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace mafia {

namespace {

enum class AttackKind { Mafia, Killer, Maniac, Shot, Blade, Kidnap, Kamikaze, Poison };

struct Attack {
    PlayerId target = 0;
    std::optional<PlayerId> attacker;
    AttackKind kind = AttackKind::Mafia;
    bool healable = true;        // доктор и защиты помогают
    bool from_commissar = false; // телохранитель и самурай «от комиссара» только спасают
    bool mafia_side = false;     // против подкидыша мафия бессильна
};

class Night {
public:
    Night(const GameState& state, std::vector<NightOrder> orders) : st_(state), orders_(std::move(orders)) {}

    NightReport run() {
        steal();
        block();
        for (const NightOrder& o : active()) {
            if (o.action.kind == NightActionKind::Slander) {
                slandered_.insert(*o.action.target);
                secret(name(o.actor) + " (баламут) оговаривает " + name(*o.action.target) + ".");
            }
        }
        protect();
        attack();
        inform();
        finish();
        return std::move(r_);
    }

private:
    // --- помощники ------------------------------------------------------------
    const PlayerState& p(PlayerId id) const { return st_.players[static_cast<std::size_t>(id)]; }
    RoleKind role(PlayerId id) const { return p(id).role; }
    Team team(PlayerId id) const { return team_of(role(id)); }
    const std::string& name(PlayerId id) const { return p(id).name; }
    bool alive(PlayerId id) const { return p(id).alive; }
    bool enemy(PlayerId id) const { return team(id) == Team::Mafia || team(id) == Team::Loner; }
    void secret(std::string line) { r_.secrets.push_back(std::move(line)); }
    // Личные сведения игроку — и строка о них в полный журнал.
    void fact(PlayerId to, PlayerId about, bool is_mafia, std::optional<RoleKind> role = std::nullopt) {
        KnownIdentity k;
        k.id = about;
        k.is_mafia = is_mafia;
        k.role = role;
        r_.facts.emplace_back(to, k);
        secret("сведения для " + name(to) + ": " + name(about) + (is_mafia ? " — враг" : " — не мафия") +
               (role ? " (" + std::string(role_name(*role)) + ")" : std::string()) + ".");
    }
    void note(PlayerId to, std::string text) {
        secret("сведения для " + name(to) + ": " + text);
        r_.notes.emplace_back(to, std::move(text));
    }

    std::vector<NightOrder> active() const {
        std::vector<NightOrder> out;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (!cancelled_.contains(i)) {
                out.push_back(orders_[i]);
            }
        }
        return out;
    }

    // --- 1. кража хода ----------------------------------------------------------
    void steal() {
        for (const NightOrder& o : std::vector<NightOrder>(orders_)) {
            if (o.action.kind != NightActionKind::Steal) {
                continue;
            }
            const PlayerId victim = *o.action.target;
            auto it = std::ranges::find_if(orders_, [&](const NightOrder& x) {
                return x.actor == victim && !x.council && x.action.kind != NightActionKind::Steal;
            });
            if (it == orders_.end()) {
                secret(name(o.actor) + " (жулик) лезет к " + name(victim) + ", но красть нечего.");
                continue;
            }
            it->actor = o.actor;  // ход совершается от имени жулика, сведения — ему
            secret(name(o.actor) + " (жулик) крадёт ход у " + name(victim) + ": " +
                   std::string(action_name(it->action.kind)) + ".");
        }
    }

    // --- 2. блокировки ----------------------------------------------------------
    void block() {
        std::set<PlayerId> blocked;
        for (const NightOrder& o : orders_) {
            if (o.action.kind == NightActionKind::Visit) {
                drunk_at_[o.actor] = *o.action.target;
                blocked.insert(*o.action.target);
                secret(name(o.actor) + " (пьяница) в гостях у " + name(*o.action.target) + ": тот пропускает ход.");
            }
        }
        for (const NightOrder& o : orders_) {
            if (o.action.kind != NightActionKind::Block) {
                continue;
            }
            if (blocked.contains(o.actor)) {
                secret(name(o.actor) + " (вор) сам заблокирован — его блокировка не сработала.");
                continue;
            }
            const PlayerId t = *o.action.target;
            if (team(t) == Team::Civilians) {
                blocked.insert(t);
                secret(name(o.actor) + " (вор) блокирует способность " + name(t) + ".");
            } else {
                secret(name(o.actor) + " (вор): " + name(t) + " не мирный — блокировка не действует.");
            }
        }
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const NightOrder& o = orders_[i];
            const bool blocking = o.action.kind == NightActionKind::Visit || o.action.kind == NightActionKind::Block;
            if (!blocking && blocked.contains(o.actor)) {
                cancelled_.insert(i);
                secret(name(o.actor) + ": ход «" + std::string(action_name(o.action.kind)) + "» сорван" +
                       (o.council ? " — убийство мафии не состоялось" : "") + ".");
            }
        }
    }

    // --- 4. защиты --------------------------------------------------------------
    void protect() {
        r_.poisonings = st_.extra.poisonings;
        for (const NightOrder& o : active()) {
            const PlayerId t = o.action.target.value_or(-1);
            switch (o.action.kind) {
                case NightActionKind::Heal:
                    healed_.insert(t);
                    secret(name(o.actor) + " (доктор) лечит " + name(t) + ".");
                    break;
                case NightActionKind::Guard:
                    (o.role == RoleKind::Samurai ? samurai_ : bodyguard_)[t].push_back(o.actor);
                    secret(name(o.actor) + " (" + std::string(role_name(o.role)) + ") охраняет " + name(t) + ".");
                    break;
                case NightActionKind::Check:
                    if (o.role == RoleKind::ReviveCommissar) {
                        shielded_.insert(t);  // проверенного в ту же ночь убить нельзя
                    }
                    break;
                default:
                    break;
            }
        }
        for (const NightOrder& o : active()) {
            if (o.action.kind == NightActionKind::Antidote) {
                for (Poisoning& pz : r_.poisonings) {
                    if (pz.by == o.actor && pz.target == *o.action.target && pz.night == night() - 1) {
                        pz.neutralized = true;
                        secret(name(o.actor) + " (отравитель) даёт противоядие " + name(pz.target) + ".");
                    }
                }
            }
            if (o.action.kind == NightActionKind::Poison) {
                Poisoning pz;
                pz.by = o.actor;
                pz.target = *o.action.target;
                pz.night = night();
                pz.neutralized = healed_.contains(pz.target);  // доктор пришёл в ночь отравления
                r_.poisonings.push_back(pz);
                secret(name(o.actor) + " (отравитель) травит " + name(pz.target) +
                       (pz.neutralized ? " — но доктор успел вылечить." : "."));
            }
        }
    }

    // --- 5. нападения -----------------------------------------------------------
    void attack() {
        std::vector<Attack> attacks;
        for (const NightOrder& o : active()) {
            if (!o.action.target) {
                continue;
            }
            const PlayerId t = *o.action.target;
            Attack a;
            a.target = t;
            a.attacker = o.actor;
            a.mafia_side = team_of(o.role) == Team::Mafia;
            switch (o.action.kind) {
                case NightActionKind::Kill:
                    a.kind = o.council ? AttackKind::Mafia
                             : o.role == RoleKind::Killer ? AttackKind::Killer
                             : o.role == RoleKind::Blade  ? AttackKind::Blade
                                                          : AttackKind::Maniac;
                    attacks.push_back(a);
                    secret(name(o.actor) + (o.council ? " исполняет решение мафии: " : " нападает на ") + name(t) + ".");
                    break;
                case NightActionKind::Shoot:
                    a.kind = AttackKind::Shot;
                    a.from_commissar = o.role == RoleKind::Commissar || o.role == RoleKind::Cattani;
                    attacks.push_back(a);
                    secret(name(o.actor) + " стреляет в " + name(t) + ".");
                    break;
                case NightActionKind::Kidnap:
                    if (o.action.guess == role(t)) {
                        a.kind = AttackKind::Kidnap;
                        a.healable = false;  // «не может вылечить от доп. удара мафии»
                        attacks.push_back(a);
                        secret(name(o.actor) + " (похититель) угадал: " + name(t) + " — " +
                               std::string(role_name(role(t))) + ".");
                    } else {
                        self_deaths_.insert(o.actor);
                        secret(name(o.actor) + " (похититель) ошибся насчёт " + name(t) + " и погибает сам.");
                    }
                    break;
                case NightActionKind::Seek:
                    if (role(t) == RoleKind::Commissar) {
                        a.kind = AttackKind::Kamikaze;
                        a.healable = false;
                        attacks.push_back(a);
                        self_deaths_.insert(o.actor);
                        secret(name(o.actor) + " (камикадзе) нашёл комиссара " + name(t) + " — гибнут оба.");
                    } else {
                        note(o.actor, name(t) + " — не комиссар.");
                    }
                    break;
                default:
                    break;
            }
        }
        // Яд прошлой ночи действует сейчас.
        for (const Poisoning& pz : r_.poisonings) {
            if (pz.night == night() - 1 && !pz.neutralized && alive(pz.target)) {
                Attack a;
                a.target = pz.target;
                a.attacker = pz.by;
                a.kind = AttackKind::Poison;
                a.healable = false;
                attacks.push_back(a);
                secret("яд отравителя " + name(pz.by) + " действует на " + name(pz.target) + ".");
            }
        }

        for (const Attack& a : attacks) {
            resolve(a);
        }
    }

    bool immune(PlayerId t, const Attack& a) const {
        const RoleKind r = role(t);
        return r == RoleKind::Blade ||                                   // блейда ночью убить нельзя
               (r == RoleKind::Bull && a.kind == AttackKind::Maniac) ||  // бык неуязвим для маньяка
               (r == RoleKind::Foundling && a.mafia_side);               // подкидыша мафия не трогает
    }

    void kill(PlayerId who, const std::string& why) {
        if (role(who) == RoleKind::Blade || st_.extra.jailed.contains(who)) {
            return;  // ночью неуязвим
        }
        dead_.insert(who);
        secret(why);
    }

    void resolve(const Attack& a) {
        const PlayerId t = a.target;
        if (!alive(t)) {
            return;
        }
        if (a.attacker) {
            attackers_[t].push_back(*a.attacker);
        }
        if (drunk_at_.contains(t) && drunk_at_.at(t) != t) {
            secret(name(t) + " (пьяница) в гостях — нападение не застало его дома.");
            return;
        }
        if (st_.extra.jailed.contains(t)) {
            saved_.insert(t);
            secret(name(t) + " в тюрьме — нападение не состоялось.");
            return;
        }
        if (immune(t, a)) {
            secret(name(t) + " (" + std::string(role_name(role(t))) + ") неуязвим для этого нападения.");
            return;
        }
        bool dies = !a.healable;
        if (!dies) {
            auto sam = samurai_.find(t);
            auto bg = bodyguard_.find(t);
            if (sam != samurai_.end() && !a.from_commissar) {
                const PlayerId s = sam->second.front();
                kill(s, name(s) + " (самурай) гибнет вместо " + name(t) + ".");
                if (a.attacker) {
                    kill(*a.attacker, name(*a.attacker) + " убит самураем.");
                }
                saved_.insert(t);
            } else if (bg != bodyguard_.end()) {
                saved_.insert(t);
                secret(name(t) + " спасён телохранителем " + name(bg->second.front()) + ".");
                if (a.attacker && !a.from_commissar) {
                    kill(*a.attacker, name(*a.attacker) + " убит телохранителем.");
                }
            } else if (healed_.contains(t) || shielded_.contains(t)) {
                saved_.insert(t);
                secret(name(t) + " спасён (" + std::string(healed_.contains(t) ? "доктор" : "проверка реанимирующего комиссара") + ").");
            } else {
                dies = true;
            }
        }
        if (!dies) {
            return;
        }
        dead_.insert(t);
        if (a.attacker) {
            killed_by_.emplace(t, role(*a.attacker));  // первое смертельное нападение
        }
        for (const auto& [drunk, host] : drunk_at_) {
            if (host == t && drunk != t) {
                dead_.insert(drunk);  // собутыльник гибнет вместе с хозяином
                secret(name(drunk) + " (пьяница) гибнет вместе с хозяином.");
            }
        }
        if (a.attacker && (a.kind == AttackKind::Blade || a.kind == AttackKind::Poison) &&
            team(t) == Team::Civilians) {
            ++honest_kills_[*a.attacker];
        }
    }

    // --- 6. сведения ------------------------------------------------------------
    void inform() {
        for (const NightOrder& o : active()) {
            if (!o.action.target) {
                continue;
            }
            const PlayerId to = o.actor;
            const PlayerId t = *o.action.target;
            const bool looks_mafia = appears_mafia(st_, t, slandered_.contains(t));
            switch (o.action.kind) {
                case NightActionKind::Check:
                    check(o, to, t, looks_mafia);
                    break;
                case NightActionKind::Compare: {
                    const PlayerId s = *o.action.second;
                    const bool same = (team(t) == Team::Mafia) == (team(s) == Team::Mafia);
                    note(to, "Журналист: у " + name(t) + " и " + name(s) + (same ? " одинаковый" : " разный") + " статус.");
                    secret(name(to) + " (журналист) сравнивает " + name(t) + " и " + name(s) + ".");
                    break;
                }
                case NightActionKind::Listen: {
                    std::string heard;
                    for (const NightOrder& x : active()) {
                        if (x.action.target == t && x.actor != to) {
                            heard += (heard.empty() ? "" : ", ") + std::string(action_name(x.action.kind));
                        }
                    }
                    note(to, "Ушастик у " + name(t) + ": " + (heard.empty() ? "тишина." : heard + "."));
                    break;
                }
                case NightActionKind::Watch:
                    if (auto it = attackers_.find(t); it != attackers_.end()) {
                        for (PlayerId k : it->second) {
                            fact(to, k, enemy(k), role(k));
                            note(to, "Свидетель: на " + name(t) + " напал " + name(k) + ".");
                        }
                    } else {
                        note(to, "Свидетель: на " + name(t) + " никто не нападал.");
                    }
                    break;
                case NightActionKind::Admire:
                    if (role(t) == RoleKind::Commissar) {
                        fact(to, t, false, RoleKind::Commissar);
                        r_.fan_done.push_back(to);
                    } else {
                        note(to, name(t) + " — не комиссар.");
                    }
                    break;
                case NightActionKind::Hack: {
                    Disclosure d;
                    d.is_mafia = team(t) == Team::Mafia;
                    d.role = role(t);
                    r_.reveals.emplace_back(t, d);
                    secret(name(to) + " (хакер) взламывает " + name(t) + ".");
                    break;
                }
                case NightActionKind::Prank:
                    r_.prank = t;
                    secret(name(to) + " (громила) разыгрывает " + name(t) + ".");
                    break;
                case NightActionKind::Freeze:
                    r_.freeze = t;
                    secret(name(to) + " (босс) замораживает " + name(t) + ".");
                    break;
                case NightActionKind::Revive:
                    if (!alive(t)) {
                        r_.revived.push_back(t);
                        secret(name(to) + " (реаниматор) воскрешает " + name(t) + ".");
                    }
                    break;
                default:
                    break;
            }
        }
    }

    void check(const NightOrder& o, PlayerId to, PlayerId t, bool looks_mafia) {
        secret(name(to) + " (" + std::string(role_name(o.role)) + ") проверяет " + name(t) + ": " +
               (looks_mafia ? "мафия." : "не мафия."));
        switch (o.role) {
            case RoleKind::Spy:
                // Результат шпиона ведущий передаёт комиссару, а не шпиону.
                for (const PlayerState& c : st_.players) {
                    if (c.alive && c.role == RoleKind::Commissar) {
                        fact(c.id, t, looks_mafia);
                    }
                }
                return;
            case RoleKind::Priest:
                fact(to, t, looks_mafia);
                fact(t, to, false, RoleKind::Priest);  // исповедуемый узнаёт священника
                return;
            case RoleKind::Donor:
                fact(to, t, looks_mafia);
                if (!looks_mafia && team(t) == Team::Civilians) {
                    r_.granted_check.push_back(t);
                }
                return;
            case RoleKind::Jailer: {
                const bool dishonest = looks_mafia || team(t) == Team::Loner;
                fact(to, t, dishonest, team(t) == Team::Loner ? std::optional(role(t)) : std::nullopt);
                if (dishonest && !st_.extra.jailed.contains(t)) {
                    r_.jailed.push_back(t);
                }
                return;
            }
            case RoleKind::Advocate:
                fact(to, t, team(t) == Team::Mafia, role(t));  // полный статус, в том числе маньяка
                return;
            default:
                break;
        }
        // Комиссар, Каттани, реанимирующий комиссар, дар донора.
        fact(to, t, looks_mafia);
        if (o.role == RoleKind::Commissar) {
            for (const PlayerState& s : st_.players) {
                if (s.alive && s.role == RoleKind::Sergeant) {
                    fact(s.id, t, looks_mafia);  // сержант знает проверки комиссара
                }
            }
            if (role(t) == RoleKind::Paranoid) {
                note(t, "Этой ночью вас проверял комиссар.");
            }
        }
        if (o.role == RoleKind::Cattani && dead_.contains(t)) {
            for (PlayerId k : attackers_[t]) {
                fact(to, k, enemy(k), role(k));  // Каттани узнаёт, кто и чем убил
                note(to, "Каттани: " + name(t) + " убит игроком " + name(k) + ".");
            }
        }
    }

    // --- 7. итог ----------------------------------------------------------------
    void finish() {
        for (const auto& [actor, added] : honest_kills_) {
            r_.honest_kills.emplace_back(actor, added);
            const int total = added + (st_.extra.honest_kills.contains(actor) ? st_.extra.honest_kills.at(actor) : 0);
            if ((role(actor) == RoleKind::Blade && total >= 3) || (role(actor) == RoleKind::Poisoner && total >= 2)) {
                self_deaths_.insert(actor);
                secret(name(actor) + " убил слишком много мирных и погибает сам.");
            }
        }
        for (PlayerId s : self_deaths_) {
            dead_.insert(s);
        }
        for (PlayerId d : dead_) {
            saved_.erase(d);
        }
        r_.deaths.assign(dead_.begin(), dead_.end());
        for (PlayerId d : dead_) {
            if (auto it = killed_by_.find(d); it != killed_by_.end()) {
                r_.killed_by.emplace_back(d, it->second);
            }
        }
        r_.saved.assign(saved_.begin(), saved_.end());
    }

    int night() const { return st_.day; }

    const GameState& st_;
    std::vector<NightOrder> orders_;
    std::set<std::size_t> cancelled_;
    std::map<PlayerId, PlayerId> drunk_at_;  // пьяница -> у кого в гостях
    std::set<PlayerId> slandered_;
    std::set<PlayerId> healed_;
    std::set<PlayerId> shielded_;
    std::map<PlayerId, std::vector<PlayerId>> bodyguard_;
    std::map<PlayerId, std::vector<PlayerId>> samurai_;
    std::map<PlayerId, std::vector<PlayerId>> attackers_;
    std::map<PlayerId, int> honest_kills_;
    std::set<PlayerId> self_deaths_;
    std::set<PlayerId> dead_;
    std::set<PlayerId> saved_;
    std::map<PlayerId, RoleKind> killed_by_;
    NightReport r_;
};

}  // namespace

bool appears_mafia(const GameState& state, PlayerId id, bool slandered) {
    if (slandered) {
        return true;
    }
    const RoleKind r = state.players[static_cast<std::size_t>(id)].role;
    if (r == RoleKind::Ninja || r == RoleKind::Werewolf) {
        return false;  // невидимка и спящий оборотень выглядят мирными
    }
    return team_of(r) == Team::Mafia;
}

NightReport resolve_full_night(const GameState& state, std::vector<NightOrder> orders) {
    return Night(state, std::move(orders)).run();
}

}  // namespace mafia
