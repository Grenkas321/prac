// Концепты способностей ролей.
//
// Концепт — это проверяемое при компиляции утверждение о ТИПЕ: «у класса есть
// такой-то метод с такой-то сигнатурой, такая-то константа, и она такая-то».
// Роли с нужной способностью удовлетворяют концепту, остальные — нет;
// дальше по этому строятся таблица способностей (catalog.hpp), фабрика ролей
// и решения ведущего (кому слать ночной запрос, кому доставлять проверку).
//
// Иерархия концептов:
//
//   RoleClass ──┬── NightActor ──┬── Inspector   (+ SecretReceiver, есть Check)
//               │                ├── Protector   (есть Heal, есть правило выбора цели)
//               │                └── Killer      (есть Kill или Shoot)
//               ├── SecretReceiver
//               ├── MafiaMember
//               └── Loner
//   ConsistentRole — RoleClass, согласованный с правилами игры.
#pragma once

#include "game/protocol.hpp"
#include "game/state.hpp"
#include "game/types.hpp"
#include "roles/player.hpp"

#include <concepts>
#include <cstddef>

namespace mafia {

namespace detail {

// Есть ли действие среди ночных действий роли R. constexpr: вызывается
// внутри концептов, то есть во время компиляции.
template <class R>
constexpr bool offers(NightActionKind action) {
    for (NightActionKind a : R::kNightActions) {
        if (a == action) {
            return true;
        }
    }
    return false;
}

}  // namespace detail

// Любая роль: наследник Player, создаётся фабрикой из PlayerSetup, знает свой
// вид, голосует, говорит, оставляет последнее слово.
template <class R>
concept RoleClass = std::derived_from<R, Player> && std::constructible_from<R, PlayerSetup> &&
                    requires(R& role, const Request& request) {
    { R::kKind } -> std::convertible_to<RoleKind>;
    { role.vote(request) } -> std::same_as<Response>;
    { role.speak(request) } -> std::same_as<Response>;
    { role.last_word(request) } -> std::same_as<Response>;
};

// Роль с ночным ходом: непустой список ночных действий, признак «можно ли
// пропустить ход» и сам ход act().
template <class R>
concept NightActor = RoleClass<R> && requires(R& role, const Request& request) {
    { R::kNightActions.size() } -> std::convertible_to<std::size_t>;
    { R::kNightActions[0] } -> std::convertible_to<NightActionKind>;
    { R::kMaySkipNight } -> std::convertible_to<bool>;
    { role.act(request) } -> std::same_as<Response>;
    requires R::kNightActions.size() > 0;  // пустой список — это не ночная роль
};

// Роль, получающая тайные сведения от ведущего: союзников, результаты проверок.
template <class R>
concept SecretReceiver = RoleClass<R> && requires(R& role, const KnownIdentity& fact) {
    { role.learn(fact) } -> std::same_as<void>;
};

// Роль, которая кого-то проверяет: ходит ночью, умеет Check и умеет
// принять результат проверки (иначе ведущему некуда его доставить).
template <class R>
concept Inspector = NightActor<R> && SecretReceiver<R> && detail::offers<R>(NightActionKind::Check);

// Роль, которая кого-то защищает: ходит ночью, умеет Heal и объявляет правило,
// кого ей можно защищать (ведущий по нему фильтрует цели).
template <class R>
concept Protector =
    NightActor<R> && detail::offers<R>(NightActionKind::Heal) &&
    requires(const GameState& state, PlayerId self, PlayerId target) {
        { R::may_protect(state, self, target) } -> std::same_as<bool>;
    };

// Роль, которая может убить ночью: убийство, выстрел или яд.
template <class R>
concept Killer = NightActor<R> && (detail::offers<R>(NightActionKind::Kill) ||
                                   detail::offers<R>(NightActionKind::Shoot) ||
                                   detail::offers<R>(NightActionKind::Poison));

// Роль, входящая в мафию.
template <class R>
concept MafiaMember = RoleClass<R> && (team_of(R::kKind) == Team::Mafia);

// Одиночка.
template <class R>
concept Loner = RoleClass<R> && (team_of(R::kKind) == Team::Loner);

// Роль, согласованная с правилами игры:
//   мафиози обязан узнавать союзников и уметь убивать;
//   одиночка обязан уметь убивать (иначе ему не победить).
template <class R>
concept ConsistentRole = RoleClass<R> &&
                         (!MafiaMember<R> || (SecretReceiver<R> && Killer<R>)) &&
                         (!Loner<R> || Killer<R>);

}  // namespace mafia
