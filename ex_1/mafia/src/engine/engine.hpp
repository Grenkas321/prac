// Слой исполнения: КАК игроки получают запросы и возвращают ответы.
//
// Ведущий знает только этот интерфейс. Нити под ним или корутины (второй
// движок) — ему всё равно: правила игры от способа исполнения не зависят.
#pragma once

#include "core/shared_ptr.hpp"
#include "game/protocol.hpp"
#include "roles/player.hpp"

#include <string_view>
#include <vector>

namespace mafia {

class Engine {
public:
    virtual ~Engine() = default;

    // Запустить исполнение. players[i]->id() == i. Движок разделяет владение
    // игроками (SharedPtr), пока не остановлен.
    virtual void start(const std::vector<SharedPtr<Player>>& players) = 0;

    // Разослать запросы (не больше одного на игрока; адресат — request.you.self)
    // и дождаться ВСЕХ ответов. Ответы — в порядке запросов, а не готовности.
    //
    // Возврат — барьер фазы: к этому моменту каждый адресат закончил handle(),
    // и всё, что он сделал, видно вызывающему. Если игрок бросил исключение,
    // оно перебрасывается отсюда — но только после того, как ответили остальные.
    virtual std::vector<Response> exchange(std::vector<Request> requests) = 0;

    // Остановить исполнение и освободить ресурсы. Повторный вызов безвреден.
    virtual void stop() = 0;

    // Человекочитаемое название движка — для журнала партии.
    virtual std::string_view name() const = 0;
};

}  // namespace mafia
