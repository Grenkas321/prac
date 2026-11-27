// Тесты CoroEngine и главное требование этапа: при одинаковом зерне
// ThreadEngine и CoroEngine дают полностью одинаковую партию.
#include "engine/coro_engine.hpp"
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <sstream>
#include <stdexcept>
#include <thread>

using namespace mafia;

namespace {

// Запоминает нить, в которой его вызвали.
class Probe : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Civilian;
    explicit Probe(PlayerSetup setup) : Player(std::move(setup), kKind) {}
    std::thread::id seen_thread;
    int calls = 0;

protected:
    Statement statement(const Request& request) override {
        seen_thread = std::this_thread::get_id();
        ++calls;
        return Statement{"Игрок " + std::to_string(id() + 1) + " на запрос " + std::to_string(request.id), std::nullopt};
    }
};

class Exploder : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Civilian;
    explicit Exploder(PlayerSetup setup) : Player(std::move(setup), kKind) {}

protected:
    Statement statement(const Request&) override {
        if (id() == 1) {
            throw std::runtime_error("игрок 2 сломался");
        }
        return Statement{"в порядке", std::nullopt};
    }
};

template <class R>
std::vector<SharedPtr<Player>> seat(int n) {
    std::vector<SharedPtr<Player>> players;
    for (PlayerId i = 0; i < n; ++i) {
        players.push_back(PlayerFactory::create_as<R>(
            PlayerSetup(i, "Игрок " + std::to_string(i + 1), Rng::stream(1, static_cast<std::uint64_t>(i) + 1))));
    }
    return players;
}

std::vector<Request> speeches(int n, std::uint64_t first_id) {
    auto world = make_shared_ptr<PublicSnapshot>();
    for (PlayerId i = 0; i < n; ++i) {
        PublicPlayer p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        world->players.push_back(p);
    }
    SharedPtr<const PublicSnapshot> shared = std::move(world);
    std::vector<Request> out;
    for (PlayerId i = n - 1; i >= 0; --i) {  // нарочно в обратном порядке
        Request r;
        r.id = first_id + static_cast<std::uint64_t>(i);
        r.kind = RequestKind::Speech;
        r.world = shared;
        r.you.self = i;
        out.push_back(r);
    }
    return out;
}

// Полная партия; из протокола убрана строка с названием движка — единственная,
// которая обязана отличаться.
struct Game {
    std::string protocol;
    GameState state;
};

Game play(SharedPtr<Engine> engine, int players, std::uint64_t seed) {
    HostConfig c;
    c.players = players;
    c.seed = seed;
    std::ostringstream out;
    Host host(c, engine, out);
    host.setup();
    host.play();
    host.shutdown();
    std::istringstream in(out.str());
    std::string protocol;
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with("Исполнение: ")) {
            protocol += line + "\n";
        }
    }
    return Game{protocol, host.state()};
}

bool same_night_log(const GameState& a, const GameState& b) {
    if (a.night_log.size() != b.night_log.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.night_log.size(); ++i) {
        const NightRecord& x = a.night_log[i];
        const NightRecord& y = b.night_log[i];
        if (x.night != y.night || x.actor != y.actor || x.action.kind != y.action.kind ||
            x.action.target != y.action.target) {
            return false;
        }
    }
    return true;
}

}  // namespace

void answers_in_request_order_one_thread() {
    std::vector<SharedPtr<Player>> players = seat<Probe>(5);
    CoroEngine engine;
    engine.start(players);
    std::vector<Response> answers = engine.exchange(speeches(5, 100));
    CHECK(answers.size() == 5);
    for (std::size_t k = 0; k < answers.size(); ++k) {
        const PlayerId expected = 4 - static_cast<PlayerId>(k);  // порядок — как у запросов
        CHECK(answers[k].from == expected);
        CHECK(answers[k].request_id == 100 + static_cast<std::uint64_t>(expected));
    }
    // Все корутины исполнялись в вызывающей нити — других нитей нет.
    for (const auto& p : players) {
        CHECK(static_cast<Probe*>(p.get())->seen_thread == std::this_thread::get_id());
    }
    engine.exchange(speeches(5, 200));  // корутина живёт между фазами
    CHECK(static_cast<Probe*>(players[0].get())->calls == 2);
}

void exception_reaches_caller_and_coroutine_survives() {
    CoroEngine engine;
    engine.start(seat<Exploder>(3));
    bool caught = false;
    try {
        engine.exchange(speeches(3, 1));
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "игрок 2 сломался";
    }
    CHECK(caught);
    std::vector<Request> again = speeches(3, 10);  // [2, 1, 0]
    std::vector<Response> ok = engine.exchange({again[0], again[2]});
    CHECK(ok.size() == 2 && ok[0].speech == "в порядке" && ok[1].from == 0);
}

void stop_destroys_frames() {
    std::vector<SharedPtr<Player>> players = seat<Probe>(4);
    {
        CoroEngine engine;
        engine.start(players);
        // Кадр корутины держит копию SharedPtr<Player> (аргумент корутины).
        CHECK(players[0].use_count() == 2);
        engine.stop();
        // destroy() освободил кадры — с ними и их копии указателей.
        CHECK(players[0].use_count() == 1);
        CHECK(!engine.running());
        engine.stop();  // повторно — безвредно
    }
    CoroEngine engine;
    engine.start(players);
    CHECK(players[0].use_count() == 2);
    // stop не вызывали — деструктор движка сделает это сам.
}

void bad_requests_rejected() {
    CoroEngine engine;
    bool not_started = false;
    try {
        engine.exchange(speeches(2, 1));
    } catch (const std::logic_error&) {
        not_started = true;
    }
    CHECK(not_started);
    engine.start(seat<Probe>(2));
    std::vector<Request> r = speeches(2, 1);
    bool duplicate = false;
    try {
        engine.exchange({r[0], r[0]});
    } catch (const std::invalid_argument&) {
        duplicate = true;
    }
    CHECK(duplicate);
}

void engines_play_identical_games() {
    int games = 0;
    for (int n : {5, 6, 8, 10, 12, 16}) {
        for (std::uint64_t seed = 1; seed <= 10; ++seed) {
            const Game threads = play(make_shared_ptr<ThreadEngine>(), n, seed);
            const Game coros = play(make_shared_ptr<CoroEngine>(), n, seed);
            ++games;
            CHECK(threads.protocol == coros.protocol);  // каждая речь, голос, смерть и итог
            CHECK(same_night_log(threads.state, coros.state));  // и все тайные ночные ходы
            CHECK(threads.state.winner == coros.state.winner && threads.state.day == coros.state.day);
            CHECK(threads.state.history.size() == coros.state.history.size());
        }
    }
    CHECK(games == 60);
    // Сравнение не пустое: разные зёрна дают разные партии и на одном движке.
    CHECK(play(make_shared_ptr<CoroEngine>(), 8, 1).protocol != play(make_shared_ptr<CoroEngine>(), 8, 2).protocol);
}

int main() {
    std::cout << "CoroEngine\n";
    test::run("ответы в порядке запросов, всё в одной нити", &answers_in_request_order_one_thread);
    test::run("исключение доходит до вызывающего, корутина жива", &exception_reaches_caller_and_coroutine_survives);
    test::run("stop и деструктор уничтожают кадры корутин", &stop_destroys_frames);
    test::run("неверные вызовы отвергаются", &bad_requests_rejected);
    test::run("нити и корутины: 60 партий совпадают полностью", &engines_play_identical_games);
    return test::finish();
}
