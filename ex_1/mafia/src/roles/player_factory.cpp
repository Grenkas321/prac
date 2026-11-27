#include "roles/player_factory.hpp"

#include <array>
#include <utility>

namespace mafia {

namespace {

using Maker = SharedPtr<Player> (*)(PlayerSetup);

// Строка i — создатель роли i. Строится при компиляции перебором kAllRoles:
// новая роль из RoleClassOf попадает сюда сама, вручную ничего не дописывать.
constexpr std::array<Maker, kRoleCount> kMakers = []<std::size_t... I>(std::index_sequence<I...>) {
    return std::array<Maker, kRoleCount>{&PlayerFactory::create_as<RoleClassOf_t<kAllRoles[I]>>...};
}(std::make_index_sequence<kRoleCount>{});

}  // namespace

SharedPtr<Player> PlayerFactory::create(RoleKind kind, PlayerSetup setup) {
    return kMakers[static_cast<std::size_t>(kind)](std::move(setup));
}

}  // namespace mafia
