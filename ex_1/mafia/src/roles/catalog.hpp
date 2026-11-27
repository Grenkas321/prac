// Каталог ролей: связь «вид роли (RoleKind) -> класс роли -> способности».
//
// Концепты отвечают на вопросы о ТИПЕ при компиляции. Но во время игры у
// ведущего на руках только RoleKind из GameState — тип ему неизвестен.
// Мост между мирами — таблица RoleCapabilities: она вычисляется концептами
// при компиляции (constexpr) для КАЖДОЙ роли из kAllRoles и читается
// ведущим во время игры. Новая роль попадает в таблицу автоматически —
// достаточно указать её класс в RoleClassOf.
#pragma once

#include "roles/civilian.hpp"
#include "roles/commissar.hpp"
#include "roles/concepts.hpp"
#include "roles/doctor.hpp"
#include "roles/extra_roles.hpp"
#include "roles/mafioso.hpp"
#include "roles/maniac.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace mafia {

// RoleKind -> класс роли. Специализация есть для каждого вида; для нового
// вида без специализации RoleClassOf<K>::type не существует — ошибка сборки.
template <RoleKind K>
struct RoleClassOf;

#define MAFIA_ROLE_CLASS(KIND, CLASS)     \
    template <>                           \
    struct RoleClassOf<RoleKind::KIND> {  \
        using type = CLASS;               \
    }

MAFIA_ROLE_CLASS(Civilian, Civilian);
MAFIA_ROLE_CLASS(Mafioso, Mafioso);
MAFIA_ROLE_CLASS(Commissar, Commissar);
MAFIA_ROLE_CLASS(Doctor, Doctor);
MAFIA_ROLE_CLASS(Maniac, Maniac);
MAFIA_ROLE_CLASS(ReviveCommissar, ReviveCommissar);
MAFIA_ROLE_CLASS(Jailer, Jailer);
MAFIA_ROLE_CLASS(Priest, Priest);
MAFIA_ROLE_CLASS(Journalist, Journalist);
MAFIA_ROLE_CLASS(Donor, Donor);
MAFIA_ROLE_CLASS(Cattani, Cattani);
MAFIA_ROLE_CLASS(Blade, Blade);
MAFIA_ROLE_CLASS(Executioner, Executioner);
MAFIA_ROLE_CLASS(Ears, Ears);
MAFIA_ROLE_CLASS(Prankster, Prankster);
MAFIA_ROLE_CLASS(Reanimator, Reanimator);
MAFIA_ROLE_CLASS(Bodyguard, Bodyguard);
MAFIA_ROLE_CLASS(Samurai, Samurai);
MAFIA_ROLE_CLASS(Witness, Witness);
MAFIA_ROLE_CLASS(Elder, Elder);
MAFIA_ROLE_CLASS(Sergeant, Sergeant);
MAFIA_ROLE_CLASS(Fan, Fan);
MAFIA_ROLE_CLASS(Spy, Spy);
MAFIA_ROLE_CLASS(Necromancer, Necromancer);
MAFIA_ROLE_CLASS(Drunk, Drunk);
MAFIA_ROLE_CLASS(Thief, Thief);
MAFIA_ROLE_CLASS(Killer, Hitman);
MAFIA_ROLE_CLASS(Advocate, Advocate);
MAFIA_ROLE_CLASS(Slanderer, Slanderer);
MAFIA_ROLE_CLASS(Ninja, Ninja);
MAFIA_ROLE_CLASS(Kamikaze, Kamikaze);
MAFIA_ROLE_CLASS(Boss, Boss);
MAFIA_ROLE_CLASS(Kidnapper, Kidnapper);
MAFIA_ROLE_CLASS(Foundling, Foundling);
MAFIA_ROLE_CLASS(Bull, Bull);
MAFIA_ROLE_CLASS(Paranoid, Paranoid);
MAFIA_ROLE_CLASS(Poisoner, Poisoner);
MAFIA_ROLE_CLASS(Hacker, Hacker);
MAFIA_ROLE_CLASS(Swindler, Swindler);
MAFIA_ROLE_CLASS(Werewolf, Werewolf);

#undef MAFIA_ROLE_CLASS

template <RoleKind K>
using RoleClassOf_t = typename RoleClassOf<K>::type;

// Способности роли в виде данных — для решений во время игры.
struct RoleCapabilities {
    RoleKind kind = RoleKind::Civilian;
    bool night_actor = false;      // слать ли ночной запрос
    bool may_skip_night = false;   // can_abstain в ночном запросе
    bool dormant = false;          // спит до превращения: ночных запросов нет
    bool inspector = false;
    bool protector = false;
    bool killer = false;
    bool receives_secrets = false;  // доставлять ли тайные сведения (союзники, проверки)
    bool mafia_member = false;
    bool loner = false;
    std::span<const NightActionKind> night_actions;  // actions в ночном запросе
};

template <RoleClass R>
constexpr RoleCapabilities capabilities_of() {
    RoleCapabilities caps;
    caps.kind = R::kKind;
    // if constexpr: для ролей без ночного хода (Civilian) этой ветки для
    // компилятора нет совсем. Обычный if не скомпилировался бы: у Civilian
    // нет ни kNightActions, ни kMaySkipNight, а обе ветки обычного if
    // обязаны быть корректны для любого R.
    if constexpr (NightActor<R>) {
        caps.night_actor = true;
        caps.may_skip_night = R::kMaySkipNight;
        caps.night_actions = R::kNightActions;
    }
    if constexpr (requires { R::kDormant; }) {
        caps.dormant = R::kDormant;
    }
    caps.inspector = Inspector<R>;
    caps.protector = Protector<R>;
    caps.killer = Killer<R>;
    caps.receives_secrets = SecretReceiver<R>;
    caps.mafia_member = MafiaMember<R>;
    caps.loner = Loner<R>;
    return caps;
}

// Таблица для ведущего: строка i — роль i. Строится при компиляции перебором
// kAllRoles: для каждой роли — её класс, для класса — концепты.
inline constexpr std::array<RoleCapabilities, kRoleCount> kCapabilities =
    []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<RoleCapabilities, kRoleCount>{capabilities_of<RoleClassOf_t<kAllRoles[I]>>()...};
    }(std::make_index_sequence<kRoleCount>{});

constexpr const RoleCapabilities& role_capabilities(RoleKind kind) {
    return kCapabilities[static_cast<std::size_t>(kind)];
}

// Полнота каталога: у каждого вида из kAllRoles есть класс, он корректная
// роль, согласован с правилами и сообщает о себе тот же вид, что и в таблице
// (защита от наследника, который забыл переопределить kKind родителя).
template <RoleKind K>
constexpr bool catalog_entry_valid() {
    using R = RoleClassOf_t<K>;
    return RoleClass<R> && ConsistentRole<R> && R::kKind == K;
}

constexpr bool catalog_complete() {
    return []<std::size_t... I>(std::index_sequence<I...>) {
        return (catalog_entry_valid<kAllRoles[I]>() && ...);
    }(std::make_index_sequence<kAllRoles.size()>{});
}

}  // namespace mafia
