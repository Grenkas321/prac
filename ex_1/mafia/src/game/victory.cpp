#include "game/victory.hpp"

namespace mafia {

AliveCount count_alive(const GameState& state) {
    AliveCount a;
    for (const PlayerState& p : state.players) {
        if (!p.alive) {
            continue;
        }
        switch (team_of(p.role)) {
            case Team::Civilians:
                ++a.civilians;
                break;
            case Team::Mafia:
                ++a.mafia;
                break;
            case Team::Loner:
                ++a.loners;
                break;
            case Team::Neutral:
                break;  // нейтральные (хакер, жулик) на исход не влияют
        }
    }
    return a;
}

Verdict judge(const AliveCount& a) {
    const int others = a.civilians + a.loners;  // все, кто не мафия (нейтральные не в счёт)
    if (a.mafia == 0 && others == 0) {
        return Verdict::Draw;
    }
    if (a.mafia == 0) {
        if (a.loners == 0) {
            return Verdict::CiviliansWin;
        }
        // Одиночка побеждает, только если он остался единственным убийцей.
        if (a.loners == 1 && a.civilians <= 1) {
            return Verdict::ManiacWin;
        }
        return Verdict::Continue;
    }
    if (a.mafia > others) {
        return Verdict::MafiaMajority;
    }
    if (a.mafia == others && a.loners == 0) {
        return Verdict::MafiaParity;
    }
    return Verdict::Continue;
}

std::optional<Team> winner_of(Verdict verdict) {
    switch (verdict) {
        case Verdict::CiviliansWin:
            return Team::Civilians;
        case Verdict::MafiaMajority:
        case Verdict::MafiaParity:
            return Team::Mafia;
        case Verdict::ManiacWin:
            return Team::Loner;
        case Verdict::Continue:
        case Verdict::Draw:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string_view winner_code(std::optional<Team> winner) {
    if (!winner) {
        return "draw";
    }
    switch (*winner) {
        case Team::Civilians:
            return "civilians";
        case Team::Mafia:
            return "mafia";
        case Team::Loner:
            return "maniac";
        case Team::Neutral:
            return "neutral";
    }
    return "draw";
}

}  // namespace mafia
