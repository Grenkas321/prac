// Полный разбор ночи со всеми ролями — чистая функция.
//
// На входе: состояние партии (кто кем является, тюрьма, яды, ...) и ВСЕ
// принятые ведущим ночные ходы. На выходе: отчёт — кто погибнет утром, кто
// спасён, кто что узнал, кто сядет в тюрьму, что объявить всем. Функция
// ничего не меняет: отчёт применяет ведущий. Порядок ходов на входе на
// результат не влияет — порядок задают этапы конвейера:
//
//   1. кража хода (жулик)          — меняет, КТО делает чужой ход
//   2. блокировки (пьяница, вор)   — снимают ходы
//   3. оговор (баламут)            — меняет, как выглядит цель при проверке
//   4. защиты (доктор, телохранитель, самурай, реанимирующий комиссар,
//      противоядие, лечение в ночь отравления)
//   5. нападения (мафия, киллер, маньяк, выстрелы, блейд, похищение,
//      камикадзе, яд прошлой ночи) — с иммунитетами и защитами
//   6. сведения (проверки, свидетель, ушастик, журналист, поклонница, хакер)
//      — уже знают, кто погиб этой ночью
//   7. гибель «по своим правилам» (блейд, отравитель, похититель, камикадзе)
#pragma once

#include "game/state.hpp"
#include "game/types.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mafia {

// Принятый ведущим ход. role — роль, по правилам которой ход толкуется
// (у украденного хода — роль хозяина), recipient — кому достаются сведения.
struct NightOrder {
    PlayerId actor = 0;
    RoleKind role = RoleKind::Civilian;
    NightAction action;
    bool council = false;  // общее решение мафии (actor — исполнитель, Босс)
};

struct NightReport {
    std::vector<PlayerId> deaths;  // погибнут утром, по возрастанию
    std::vector<PlayerId> saved;   // на них напали, но их спасли
    std::vector<PlayerId> jailed;  // новые заключённые
    std::vector<PlayerId> revived;
    std::optional<PlayerId> prank;
    std::optional<PlayerId> freeze;
    std::vector<PlayerId> granted_check;
    std::vector<PlayerId> fan_done;
    std::vector<std::pair<PlayerId, KnownIdentity>> facts;  // кому — что достоверно
    std::vector<std::pair<PlayerId, std::string>> notes;    // кому — что текстом
    std::vector<std::pair<PlayerId, Disclosure>> reveals;   // объявить всем (хакер)
    std::vector<Poisoning> poisonings;                      // новый список ядов
    std::vector<std::pair<PlayerId, int>> honest_kills;     // блейд/отравитель: +N мирных
    std::vector<std::string> secrets;                       // для полного журнала
    // Кто убил погибшего (роль нападавшего) — для открытых объявлений. Есть не
    // у всех: гибель «по своим правилам» и собутыльник пьяницы — без убийцы.
    std::vector<std::pair<PlayerId, RoleKind>> killed_by;
};

NightReport resolve_full_night(const GameState& state, std::vector<NightOrder> orders);

// Как игрок выглядит при проверке «мафия или нет» (ниндзя и оборотень —
// мирными, оговорённый баламутом — мафией).
bool appears_mafia(const GameState& state, PlayerId id, bool slandered);

}  // namespace mafia
