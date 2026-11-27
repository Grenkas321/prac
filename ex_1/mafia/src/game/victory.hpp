// Условия конца игры — чистые функции над составом живых.
//
//   M — живые мафиози, C — живые мирные (включая комиссара и доктора),
//   X — живые одиночки-убийцы (маньяк, отравитель). «Остальные» для мафии — C + X.
//   Нейтральные (хакер, жулик) не считаются нигде.
//
//   никого нет                               -> ничья
//   M = 0 и X = 0                            -> победа мирных
//   M = 0, X = 1, C <= 1                     -> победа одиночки
//   M > C + X                                -> победа мафии
//   M = C + X и X = 0                        -> победа мафии (паритет)
//   иначе                                    -> игра продолжается
//
// В частности, M = C + X при живом маньяке — продолжение (правило 5 мафии
// из задания): у мафии паритета нет, пока жив третий стрелок.
#pragma once

#include "game/state.hpp"
#include "game/types.hpp"

#include <optional>
#include <string_view>

namespace mafia {

struct AliveCount {
    int civilians = 0;  // команда мирных
    int mafia = 0;
    int loners = 0;     // одиночки-убийцы (маньяк, отравитель)
};

AliveCount count_alive(const GameState& state);

enum class Verdict {
    Continue,
    CiviliansWin,  // мафия и маньяк выбыли
    MafiaMajority, // мафии больше, чем всех остальных
    MafiaParity,   // мафии столько же, сколько остальных, маньяка нет
    ManiacWin,     // маньяк один на один с мирным (или совсем один)
    Draw,          // за столом не осталось никого
};

Verdict judge(const AliveCount& alive);

// Победившая команда; пусто — ничья или игра не окончена.
std::optional<Team> winner_of(Verdict verdict);

// Короткий код итога для машиночитаемой строки: civilians / mafia / maniac / draw.
std::string_view winner_code(std::optional<Team> winner);

}  // namespace mafia
