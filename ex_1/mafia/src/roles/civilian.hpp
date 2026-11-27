// Мирный житель: ночью спит, тайных сведений не получает — ведёт себя
// ровно так, как база. Базовый класс для мирных со способностями
// (комиссар, доктор): они наследуют всё дневное поведение мирного.
#pragma once

#include "roles/player.hpp"

namespace mafia {

class Civilian : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Civilian;

    explicit Civilian(PlayerSetup setup) : Player(std::move(setup), kKind) {}

protected:
    // Для наследников: они передают свой вид роли.
    Civilian(PlayerSetup setup, RoleKind role) : Player(std::move(setup), role) {}
};

}  // namespace mafia
