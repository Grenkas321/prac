// Маньяк: одиночка, каждую ночь убивает любого. Тайных сведений не получает,
// днём ведёт себя как обычный игрок (голосует по своим оценкам).
// Ночью убирает самого опасного для себя: того, кто его обвинял, и того,
// кому верит город.
#pragma once

#include "roles/player.hpp"

#include <array>

namespace mafia {

class Maniac : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Maniac;
    static constexpr std::array<NightActionKind, 1> kNightActions = {NightActionKind::Kill};
    static constexpr bool kMaySkipNight = false;

    explicit Maniac(PlayerSetup setup) : Player(std::move(setup), kKind) {}

    Response act(const Request& request);

protected:
    int target_score(const Request& request, Purpose purpose, PlayerId candidate) const override;
    std::size_t choose_option(const Request& request) override;
};

}  // namespace mafia
