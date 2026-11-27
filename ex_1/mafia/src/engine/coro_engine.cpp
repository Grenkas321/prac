#include "engine/coro_engine.hpp"

#include <stdexcept>
#include <string>

namespace mafia {

// Корутина игрока. Аргументы копируются в её кадр и живут, пока жив кадр.
CoroEngine::PlayerTask CoroEngine::player_loop(SharedPtr<Player> player, SharedPtr<Channel> channel) {
    for (;;) {
        std::optional<Request> request = co_await NextRequest{*channel};
        if (!request) {
            co_return;  // остановка: дальше final_suspend
        }
        Outcome outcome;
        try {
            outcome.response = player->handle(*request);
        } catch (...) {
            // co_yield внутри catch запрещён, поэтому сначала запоминаем, потом отдаём.
            outcome.error = std::current_exception();
        }
        co_yield std::move(outcome);
    }
}

CoroEngine::~CoroEngine() {
    stop();
}

void CoroEngine::start(const std::vector<SharedPtr<Player>>& players) {
    if (running()) {
        throw std::logic_error("CoroEngine: движок уже запущен");
    }
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (!players[i] || players[i]->id() != static_cast<PlayerId>(i)) {
            throw std::invalid_argument("CoroEngine: игрок №" + std::to_string(i) +
                                        " отсутствует или его id не совпадает с местом");
        }
    }
    for (const SharedPtr<Player>& player : players) {
        SharedPtr<Channel> channel = make_shared_ptr<Channel>();
        channels_.push_back(channel);
        tasks_.push_back(player_loop(player, channel));  // создана и приостановлена (initial_suspend)
        // Первый шаг: тело доходит до co_await, запроса нет — корутина засыпает
        // в ожидании. Теперь каждая корутина «ждёт запрос», как нить ThreadEngine.
        tasks_.back().resume();
    }
}

std::vector<Response> CoroEngine::exchange(std::vector<Request> requests) {
    if (!running()) {
        throw std::logic_error("CoroEngine: движок не запущен");
    }
    // Те же проверки пакета, что у ThreadEngine — до рассылки.
    std::vector<bool> addressed(channels_.size(), false);
    for (const Request& r : requests) {
        const PlayerId to = r.you.self;
        if (to < 0 || static_cast<std::size_t>(to) >= channels_.size()) {
            throw std::invalid_argument("CoroEngine: запрос неизвестному игроку №" + std::to_string(to));
        }
        if (addressed[static_cast<std::size_t>(to)]) {
            throw std::invalid_argument("CoroEngine: два запроса игроку №" + std::to_string(to) + " в одной фазе");
        }
        addressed[static_cast<std::size_t>(to)] = true;
    }

    std::vector<Response> responses;
    responses.reserve(requests.size());
    std::exception_ptr failure;
    for (Request& r : requests) {
        const auto to = static_cast<std::size_t>(r.you.self);
        Channel& channel = *channels_[to];
        channel.request = std::move(r);
        tasks_[to].resume();  // корутина считает ответ и останавливается на co_yield
        if (!channel.reply) {
            throw std::logic_error("CoroEngine: корутина игрока №" + std::to_string(to) + " не ответила");
        }
        Outcome outcome = std::move(*channel.reply);
        channel.reply.reset();
        if (outcome.error) {
            if (!failure) {
                failure = outcome.error;  // как у нитей: ждём (опрашиваем) всех, потом бросаем
            }
            responses.emplace_back();
        } else {
            responses.push_back(std::move(outcome.response));
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    return responses;
}

void CoroEngine::stop() {
    // Разбудить ждущих с флагом остановки: co_await вернёт пусто, цикл
    // закончится co_return, корутина встанет на final_suspend.
    for (std::size_t i = 0; i < tasks_.size(); ++i) {
        channels_[i]->stopping = true;
        tasks_[i].resume();
    }
    tasks_.clear();     // ~PlayerTask -> destroy(): кадры и их SharedPtr-аргументы освобождены
    channels_.clear();
}

}  // namespace mafia
