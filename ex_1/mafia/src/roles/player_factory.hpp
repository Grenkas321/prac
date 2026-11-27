// Фабрика игроков: RoleKind -> готовый игрок в SharedPtr<Player>.
//
// Здесь — единственное место, где конкретный тип роли R ещё известен.
// Пока он известен, концепты решают, что подключить к базе:
//   NightActor<R>     -> ночной ход R::act() становится обработчиком NightMove;
//   SecretReceiver<R> -> R::learn() принимает тайные сведения из Briefing.
// Дальше тип стирается: у ведущего и движка на руках только SharedPtr<Player>.
#pragma once

#include "core/shared_ptr.hpp"
#include "roles/catalog.hpp"
#include "roles/concepts.hpp"
#include "roles/player.hpp"

namespace mafia {

class PlayerFactory {
public:
    // Тип роли известен только во время игры (раздача ролей).
    static SharedPtr<Player> create(RoleKind kind, PlayerSetup setup);

    // Тип роли известен при компиляции.
    template <RoleClass R>
    static SharedPtr<Player> create_as(PlayerSetup setup) {
        SharedPtr<R> player = make_shared_ptr<R>(std::move(setup));
        R* self = player.get();  // не владеет: обработчики живут внутри самого объекта

        // Для Civilian обе ветки отбрасываются при компиляции — у мирного
        // нет ни act(), ни learn(), и код подключения для него не создаётся.
        if constexpr (NightActor<R>) {
            player->night_move_ = [self](const Request& request) { return self->act(request); };
        }
        if constexpr (SecretReceiver<R>) {
            player->secret_sink_ = [self](const KnownIdentity& fact) { self->learn(fact); };
        }
        return player;  // SharedPtr<R> -> SharedPtr<Player>, счётчик тот же
    }
};

}  // namespace mafia
