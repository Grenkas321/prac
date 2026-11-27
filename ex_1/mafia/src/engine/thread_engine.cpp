#include "engine/thread_engine.hpp"

#include <exception>
#include <stdexcept>
#include <string>

namespace mafia {

ThreadEngine::~ThreadEngine() {
    // В том числе при раскрутке стека после исключения: нити присоединяются,
    // и std::thread не уничтожается в состоянии joinable (иначе std::terminate).
    stop();
}

void ThreadEngine::start(const std::vector<SharedPtr<Player>>& players) {
    if (running()) {
        throw std::logic_error("ThreadEngine: движок уже запущен");
    }
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (!players[i] || players[i]->id() != static_cast<PlayerId>(i)) {
            throw std::invalid_argument("ThreadEngine: игрок №" + std::to_string(i) +
                                        " отсутствует или его id не совпадает с местом");
        }
    }
    boxes_.reserve(players.size());
    threads_.reserve(players.size());
    for (const SharedPtr<Player>& player : players) {
        SharedPtr<Mailbox> box = make_shared_ptr<Mailbox>();
        boxes_.push_back(box);
        // Нить получает свои копии SharedPtr: игрок и ящик живут, пока жива нить.
        // Создание нити синхронизировано с началом её функции: всё, что ведущий
        // сделал с игроком до этого (фабрика), нить увидит.
        threads_.emplace_back(&ThreadEngine::worker_loop, player, box);
    }
}

void ThreadEngine::worker_loop(SharedPtr<Player> player, SharedPtr<Mailbox> box) {
    for (;;) {
        std::optional<Job> job;
        {
            std::unique_lock lock(box->mutex);
            // Спим, пока нет работы. Предикат защищает от ложных пробуждений
            // и от пропущенного notify (если задание положили до wait).
            box->wake.wait(lock, [&] { return box->job.has_value() || box->stopping; });
            if (!box->job) {
                return;  // остановка, работы нет
            }
            job = std::move(box->job);
            box->job.reset();
        }
        // Ответ считаем БЕЗ мьютекса: ящик свободен, ведущий не ждёт на нём.
        try {
            job->reply.set_value(player->handle(job->request));
        } catch (...) {
            // Исключение не должно убить нить (std::terminate) — передаём
            // его ведущему тем же каналом, что и ответ.
            job->reply.set_exception(std::current_exception());
        }
    }
}

std::vector<Response> ThreadEngine::exchange(std::vector<Request> requests) {
    if (!running()) {
        throw std::logic_error("ThreadEngine: движок не запущен");
    }

    // Сначала проверяем весь пакет — чтобы не разослать половину и упасть.
    std::vector<bool> addressed(boxes_.size(), false);
    for (const Request& r : requests) {
        const PlayerId to = r.you.self;
        if (to < 0 || static_cast<std::size_t>(to) >= boxes_.size()) {
            throw std::invalid_argument("ThreadEngine: запрос неизвестному игроку №" + std::to_string(to));
        }
        if (addressed[static_cast<std::size_t>(to)]) {
            throw std::invalid_argument("ThreadEngine: два запроса игроку №" + std::to_string(to) +
                                        " в одной фазе");
        }
        addressed[static_cast<std::size_t>(to)] = true;
    }

    // Разослать.
    std::vector<std::future<Response>> pending;
    pending.reserve(requests.size());
    for (Request& r : requests) {
        Mailbox& box = *boxes_[static_cast<std::size_t>(r.you.self)];
        Job job{std::move(r), std::promise<Response>()};
        pending.push_back(job.reply.get_future());
        {
            std::lock_guard lock(box.mutex);
            box.job.emplace(std::move(job));
        }
        box.wake.notify_one();  // после unlock: проснувшаяся нить не упрётся в занятый мьютекс
    }

    // Дождаться ВСЕХ. Первое исключение запоминаем, но ждём остальных:
    // барьер фазы держится даже при ошибке.
    std::vector<Response> responses;
    responses.reserve(pending.size());
    std::exception_ptr failure;
    for (std::future<Response>& f : pending) {
        try {
            responses.push_back(f.get());
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
            responses.emplace_back();
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    return responses;
}

void ThreadEngine::stop() {
    for (const SharedPtr<Mailbox>& box : boxes_) {
        {
            std::lock_guard lock(box->mutex);
            box->stopping = true;
        }
        box->wake.notify_one();
    }
    for (std::thread& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
    boxes_.clear();
}

}  // namespace mafia
