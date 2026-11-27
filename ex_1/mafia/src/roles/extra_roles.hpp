// Дополнительные роли (из конфига). Классы строятся из двух шаблонов:
//
//   ActiveRole<Spec, Base>  — роль со своим ночным ходом: Spec задаёт вид роли,
//                             её ночные действия и можно ли пропустить ход;
//   PassiveRole<Spec, Base> — роль без своего ночного хода: способность
//                             пассивная (ниндзя, бык) или дневная (палач).
//
// Base — от кого наследуем общее поведение:
//   Civilian — мирные статусы: днём ведут себя как мирные;
//   Mafioso  — специализированные мафиози: знают своих, голосуют на «совете
//              мафии» как мафиози, днём подставляют мирных;
//   Player   — одиночки и нейтральные.
//
// Ночной ход доп. роли выбирается общей политикой Player::act_by_policy по виду
// действия. Спящие роли (kDormant) получают способности позже — когда ведущий
// сменит им роль в GameState (сержант -> комиссар, подкидыш -> мафиози).
#pragma once

#include "roles/civilian.hpp"
#include "roles/mafioso.hpp"
#include "roles/player.hpp"

#include <array>
#include <type_traits>

namespace mafia {

// --- Описания способностей (Spec) -------------------------------------------

template <RoleKind K, bool MaySkip, NightActionKind... Actions>
struct Active {
    static constexpr RoleKind kKind = K;
    static constexpr std::array<NightActionKind, sizeof...(Actions)> kNightActions = {Actions...};
    static constexpr bool kMaySkipNight = MaySkip;
    static constexpr bool kDormant = false;
};

// Спит до превращения: ночных запросов не получает, пока у него эта роль.
template <RoleKind K, NightActionKind... Actions>
struct DormantActive : Active<K, false, Actions...> {
    static constexpr bool kDormant = true;
};

template <RoleKind K, bool Dormant = false>
struct Passive {
    static constexpr RoleKind kKind = K;
    static constexpr bool kDormant = Dormant;
};

// --- Шаблоны классов ----------------------------------------------------------

template <class Spec, class Base>
class ActiveRole : public Base {
public:
    static constexpr RoleKind kKind = Spec::kKind;
    static constexpr auto kNightActions = Spec::kNightActions;
    static constexpr bool kMaySkipNight = Spec::kMaySkipNight;
    static constexpr bool kDormant = Spec::kDormant;

    explicit ActiveRole(PlayerSetup setup) : Base(std::move(setup), kKind) {}

    Response act(const Request& request) {
        if constexpr (std::is_base_of_v<Mafioso, Base>) {
            // «Совет мафии»: предложено только убийство — голосуем как мафиози.
            if (request.actions.size() == 1 && request.actions.front() == NightActionKind::Kill &&
                Base::in_mafia(request)) {
                return Mafioso::act(request);
            }
        }
        return this->act_by_policy(request);
    }
};

template <class Spec, class Base>
class PassiveRole : public Base {
public:
    static constexpr RoleKind kKind = Spec::kKind;
    static constexpr bool kDormant = Spec::kDormant;

    explicit PassiveRole(PlayerSetup setup) : Base(std::move(setup), kKind) {}
};

using NA = NightActionKind;

// --- Статусы мирных жителей ---------------------------------------------------
using ReviveCommissar = ActiveRole<Active<RoleKind::ReviveCommissar, false, NA::Check>, Civilian>;
using Jailer = ActiveRole<Active<RoleKind::Jailer, false, NA::Check>, Civilian>;
using Priest = ActiveRole<Active<RoleKind::Priest, false, NA::Check>, Civilian>;
using Journalist = ActiveRole<Active<RoleKind::Journalist, false, NA::Compare>, Civilian>;
using Donor = ActiveRole<Active<RoleKind::Donor, false, NA::Check>, Civilian>;
using Cattani = ActiveRole<Active<RoleKind::Cattani, false, NA::Check, NA::Shoot>, Civilian>;
using Blade = ActiveRole<Active<RoleKind::Blade, true, NA::Kill>, Civilian>;
using Ears = ActiveRole<Active<RoleKind::Ears, false, NA::Listen>, Civilian>;
using Prankster = ActiveRole<Active<RoleKind::Prankster, false, NA::Prank>, Civilian>;

// Палач: днём, при ничьей — казнить одного из лидеров или помиловать.
class Executioner : public PassiveRole<Passive<RoleKind::Executioner>, Civilian> {
public:
    using PassiveRole::PassiveRole;

protected:
    std::size_t choose_option(const Request& request) override;
};

// --- Помощники комиссара ------------------------------------------------------
using Reanimator = ActiveRole<Active<RoleKind::Reanimator, true, NA::Revive>, Civilian>;
using Bodyguard = ActiveRole<Active<RoleKind::Bodyguard, false, NA::Guard>, Civilian>;
using Samurai = ActiveRole<Active<RoleKind::Samurai, false, NA::Guard>, Civilian>;
using Witness = ActiveRole<Active<RoleKind::Witness, false, NA::Watch>, Civilian>;
using Elder = PassiveRole<Passive<RoleKind::Elder>, Civilian>;
using Sergeant = ActiveRole<DormantActive<RoleKind::Sergeant, NA::Check, NA::Shoot>, Civilian>;
using Fan = ActiveRole<Active<RoleKind::Fan, false, NA::Admire>, Civilian>;
using Spy = ActiveRole<Active<RoleKind::Spy, false, NA::Check>, Civilian>;
using Drunk = ActiveRole<Active<RoleKind::Drunk, true, NA::Visit>, Civilian>;

// Некромант: днём, раз за игру — раскрыться и воскресить выбывшего.
class Necromancer : public PassiveRole<Passive<RoleKind::Necromancer>, Civilian> {
public:
    using PassiveRole::PassiveRole;

protected:
    std::size_t choose_option(const Request& request) override;
};

// --- Специализированные мафиози -------------------------------------------------
// У каждого в списке есть Kill — это голос на «совете мафии». Своё особое
// действие ведущий предлагает отдельным запросом.
using Thief = ActiveRole<Active<RoleKind::Thief, false, NA::Kill, NA::Block>, Mafioso>;
// «Киллер». Имя Killer уже занято концептом «роль умеет убивать» (concepts.hpp).
using Hitman = ActiveRole<Active<RoleKind::Killer, false, NA::Kill>, Mafioso>;
using Advocate = ActiveRole<Active<RoleKind::Advocate, false, NA::Kill, NA::Check, NA::Shoot>, Mafioso>;
using Slanderer = ActiveRole<Active<RoleKind::Slanderer, false, NA::Kill, NA::Slander>, Mafioso>;
using Kamikaze = ActiveRole<Active<RoleKind::Kamikaze, false, NA::Kill, NA::Seek>, Mafioso>;
using Boss = ActiveRole<Active<RoleKind::Boss, false, NA::Kill, NA::Freeze>, Mafioso>;
using Kidnapper = ActiveRole<Active<RoleKind::Kidnapper, true, NA::Kill, NA::Kidnap>, Mafioso>;
using Ninja = PassiveRole<Passive<RoleKind::Ninja>, Mafioso>;
using Bull = PassiveRole<Passive<RoleKind::Bull>, Mafioso>;
using Paranoid = PassiveRole<Passive<RoleKind::Paranoid>, Mafioso>;
// Спят до превращения в мафиози; Kill и приём сведений унаследованы от Mafioso.
using Foundling = PassiveRole<Passive<RoleKind::Foundling, true>, Mafioso>;
using Werewolf = PassiveRole<Passive<RoleKind::Werewolf, true>, Mafioso>;

// --- Одиночки и нейтральные -----------------------------------------------------
using Poisoner = ActiveRole<Active<RoleKind::Poisoner, false, NA::Poison, NA::Antidote>, Player>;
using Hacker = ActiveRole<Active<RoleKind::Hacker, false, NA::Hack>, Player>;
using Swindler = ActiveRole<Active<RoleKind::Swindler, true, NA::Steal>, Player>;

}  // namespace mafia
