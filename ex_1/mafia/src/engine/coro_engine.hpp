// Движок на корутинах C++20: каждый игрок — корутина, все — в одной нити.
//
// Корутина игрока в цикле:
//     запрос = co_await NextRequest{канал};   // ждать запрос (приостановиться)
//     если остановка — co_return;
//     co_yield ответ;                          // отдать ответ (приостановиться)
//
// Движок (та же нить, что и ведущий) кладёт запрос в канал игрока и
// возобновляет его корутину; она досчитывает до co_yield и отдаёт управление
// обратно вместе с ответом. Никаких нитей, мьютексов и условных переменных:
// в каждый момент исполняется ровно одна корутина или ведущий.
//
// Интерфейс тот же, что у ThreadEngine: ведущий и игроки о движке не знают.
#pragma once

#include "engine/engine.hpp"

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace mafia {

class CoroEngine final : public Engine {
public:
    CoroEngine() = default;
    ~CoroEngine() override;

    CoroEngine(const CoroEngine&) = delete;
    CoroEngine& operator=(const CoroEngine&) = delete;

    void start(const std::vector<SharedPtr<Player>>& players) override;
    std::vector<Response> exchange(std::vector<Request> requests) override;
    void stop() override;
    std::string_view name() const override { return "корутины: по одной на игрока, всё в одной нити"; }

    bool running() const noexcept { return !tasks_.empty(); }

private:
    // Что корутина отдаёт через co_yield: ответ или исключение игрока.
    struct Outcome {
        Response response;
        std::exception_ptr error;
    };

    // Канал обмена «движок <-> корутина игрока». Общий для двух сторон,
    // поэтому в SharedPtr; блокировок нет — обе стороны в одной нити.
    struct Channel {
        std::optional<Request> request;  // кладёт движок, забирает корутина (co_await)
        std::optional<Outcome> reply;    // кладёт корутина (co_yield), забирает движок
        bool stopping = false;
    };

    // Ожидатель «следующий запрос»: то, что стоит справа от co_await.
    struct NextRequest {
        Channel& channel;

        // Есть ли уже что взять? Да — не приостанавливаемся вовсе.
        bool await_ready() const noexcept { return channel.request.has_value() || channel.stopping; }
        // Корутина уже приостановлена; true — остаться приостановленной,
        // false — передумать и сразу продолжить (условие успело выполниться).
        bool await_suspend(std::coroutine_handle<>) const noexcept { return !await_ready(); }
        // Результат выражения co_await: запрос или пусто (остановка).
        std::optional<Request> await_resume() const {
            if (!channel.request) {
                return std::nullopt;
            }
            std::optional<Request> r = std::move(channel.request);
            channel.request.reset();
            return r;
        }
    };

    // Тип возвращаемого значения корутины. Владеет её кадром (RAII):
    // перемещаемый, не копируемый, в деструкторе — destroy().
    class PlayerTask {
    public:
        struct promise_type {
            // Конструктор обещания получает аргументы корутины (те, что уже
            // скопированы в её кадр) — так обещание узнаёт свой канал.
            promise_type(const SharedPtr<Player>&, const SharedPtr<Channel>& ch) : channel(ch.get()) {}

            PlayerTask get_return_object() {
                return PlayerTask(std::coroutine_handle<promise_type>::from_promise(*this));
            }
            // Ленивый старт: создана — и сразу приостановлена, тело ещё не выполнялось.
            std::suspend_always initial_suspend() noexcept { return {}; }
            // co_yield ответ: положить ответ в канал и приостановиться.
            std::suspend_always yield_value(Outcome outcome) {
                channel->reply = std::move(outcome);
                return {};
            }
            void return_void() noexcept {}
            // Исключение, вылетевшее из тела мимо try (в нашем цикле не бывает) —
            // отдаём движку как ответ с ошибкой.
            void unhandled_exception() noexcept { channel->reply = Outcome{Response{}, std::current_exception()}; }
            // В конце — приостановиться, а не уничтожиться самой: кадр живёт,
            // пока его не уничтожит владелец (PlayerTask), и done() можно спросить.
            std::suspend_always final_suspend() noexcept { return {}; }

            Channel* channel;  // не владеет: канал держит SharedPtr-аргумент в кадре
        };

        PlayerTask(PlayerTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
        PlayerTask& operator=(PlayerTask&& other) noexcept {
            if (this != &other) {
                reset();
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }
        PlayerTask(const PlayerTask&) = delete;
        PlayerTask& operator=(const PlayerTask&) = delete;
        ~PlayerTask() { reset(); }

        void resume() {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
        }
        bool done() const noexcept { return !handle_ || handle_.done(); }

    private:
        explicit PlayerTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
        void reset() noexcept {
            if (handle_) {
                handle_.destroy();  // освобождает кадр: локальные переменные, аргументы, обещание
                handle_ = {};
            }
        }
        std::coroutine_handle<promise_type> handle_;
    };

    static PlayerTask player_loop(SharedPtr<Player> player, SharedPtr<Channel> channel);

    std::vector<SharedPtr<Channel>> channels_;  // индекс == PlayerId
    std::vector<PlayerTask> tasks_;
};

}  // namespace mafia
