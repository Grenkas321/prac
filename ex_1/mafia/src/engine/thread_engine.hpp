// Движок на std::thread: на каждого игрока — одна долгоживущая нить.
//
// Нить игрока спит на условной переменной своего почтового ящика,
// просыпается, когда ведущий кладёт запрос, вызывает player->handle()
// и отдаёт ответ (или исключение) через std::promise. Ведущий ждёт ответы
// на std::future — это и есть барьер фазы.
//
// Что общее между нитями и чем защищено:
//   Mailbox (job, stopping)      — мьютекс ящика;
//   PublicSnapshot в запросах    — неизменяем (const), блокировки не нужны;
//   счётчики SharedPtr           — атомарные (core/shared_ptr.hpp);
//   объект Player                — НЕ общий: пока движок запущен, его трогает
//                                  только своя нить (и ведущий — после барьера);
//   GameState                    — в нити игроков не попадает вообще.
#pragma once

#include "engine/engine.hpp"

#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

namespace mafia {

class ThreadEngine final : public Engine {
public:
    ThreadEngine() = default;
    ~ThreadEngine() override;

    ThreadEngine(const ThreadEngine&) = delete;
    ThreadEngine& operator=(const ThreadEngine&) = delete;

    void start(const std::vector<SharedPtr<Player>>& players) override;
    std::vector<Response> exchange(std::vector<Request> requests) override;
    void stop() override;
    std::string_view name() const override { return "нити: по одной std::thread на игрока"; }

    bool running() const noexcept { return !threads_.empty(); }

private:
    // Одно задание: запрос и обещание ответа на него.
    struct Job {
        Request request;
        std::promise<Response> reply;
    };

    // Почтовый ящик игрока. Общий для ведущего (кладёт задание, просит
    // остановиться) и нити игрока (забирает задание) — поэтому под мьютексом.
    struct Mailbox {
        std::mutex mutex;
        std::condition_variable wake;
        std::optional<Job> job;  // не больше одного задания за фазу
        bool stopping = false;
    };

    static void worker_loop(SharedPtr<Player> player, SharedPtr<Mailbox> box);

    std::vector<SharedPtr<Mailbox>> boxes_;  // индекс == PlayerId
    std::vector<std::thread> threads_;
};

}  // namespace mafia
