// Движок-обёртка для режима --interactive: одно место за столом — пользователь.
//
// Оборачивает любой движок (нити или корутины). В каждом пакете exchange
// запрос на место пользователя не уходит боту: обёртка печатает подсказку
// ведущего — какая фаза, что требуется, какие варианты допустимы — и читает
// ответ из консоли. Остальные запросы передаются внутреннему движку как есть.
// Ответы возвращаются в порядке запросов, как требует интерфейс Engine.
//
// Почему обёртка, а не класс «игрок-человек»: человек может получить любую
// из 40 ролей, и ему нужны ровно те же запросы, что и боту этой роли. Запрос
// уже содержит всё — допустимые цели и действия, снимок, личную сводку, —
// поэтому ни ведущего, ни роли менять не нужно, и работает с обоими движками.
//
// Неверный ввод — переспросить. Конец ввода (EOF) — дальше за пользователя
// решает обычный бот его роли (объект игрока на этом месте есть всегда):
// партия не падает и доигрывается.
//
// Консоль читает и пишет только нить ведущего: exchange вызывается из неё.
#pragma once

#include "engine/engine.hpp"

#include <iosfwd>
#include <optional>
#include <string>

namespace mafia {

class ConsoleEngine final : public Engine {
public:
    ConsoleEngine(SharedPtr<Engine> inner, PlayerId seat, std::istream& in, std::ostream& out);

    void start(const std::vector<SharedPtr<Player>>& players) override;
    std::vector<Response> exchange(std::vector<Request> requests) override;
    void stop() override;
    std::string_view name() const override { return name_; }

    PlayerId seat() const noexcept { return seat_; }
    bool input_closed() const noexcept { return input_closed_; }

    // Спросить пользователя об одном запросе. Пусто — ввод закончился.
    // Отдельная функция — чтобы проверять разбор ввода без партии.
    static std::optional<Response> ask(const Request& request, std::istream& in, std::ostream& out);

private:
    SharedPtr<Engine> inner_;
    PlayerId seat_;
    std::istream& in_;
    std::ostream& out_;
    std::string name_;
    bool input_closed_ = false;
};

}  // namespace mafia
