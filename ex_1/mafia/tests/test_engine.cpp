// Тесты ThreadEngine: общий запрос нескольким нитям, порядок ответов,
// долгоживущие нити, перенос исключения в главную нить, остановка,
// отсутствие холостого цикла. Запускать и под ThreadSanitizer: make tsan.
#include "engine/thread_engine.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <ctime>
#include <set>
#include <stdexcept>
#include <thread>

using namespace mafia;
using namespace std::chrono_literals;

namespace {

// Роль-зонд: запоминает, в какой нити её вызвали, и отвечает с задержкой,
// обратной номеру, — чтобы ответы приходили в «перевёрнутом» порядке.
class Probe : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Civilian;
    explicit Probe(PlayerSetup setup) : Player(std::move(setup), kKind) {}

    // Пишет только нить игрока; ведущий читает после барьера exchange().
    std::thread::id seen_thread;
    int calls = 0;

protected:
    Statement statement(const Request& request) override {
        seen_thread = std::this_thread::get_id();
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(5 * (6 - id())));
        return Statement{"Говорит " + name() + " в ответ на запрос " + std::to_string(request.id), std::nullopt};
    }
};

// Роль, которая «ломается» на своём ходе.
class Exploder : public Player {
public:
    static constexpr RoleKind kKind = RoleKind::Civilian;
    explicit Exploder(PlayerSetup setup) : Player(std::move(setup), kKind) {}

protected:
    Statement statement(const Request&) override {
        if (id() == 2) {
            throw std::runtime_error("игрок 3 сломался");
        }
        return Statement{"Я в порядке", std::nullopt};
    }
};

static_assert(RoleClass<Probe> && RoleClass<Exploder>);

constexpr int kPlayers = 6;

template <class R>
std::vector<SharedPtr<Player>> seat(std::uint64_t seed = 1) {
    std::vector<SharedPtr<Player>> players;
    for (PlayerId i = 0; i < kPlayers; ++i) {
        players.push_back(PlayerFactory::create_as<R>(
            PlayerSetup(i, "Игрок " + std::to_string(i + 1), Rng::stream(seed, static_cast<std::uint64_t>(i) + 1))));
    }
    return players;
}

// Один снимок на всех — как будет делать ведущий.
std::vector<Request> speech_round(std::uint64_t first_id) {
    auto world = make_shared_ptr<PublicSnapshot>();
    world->day = 1;
    world->phase = Phase::Discussion;
    for (PlayerId i = 0; i < kPlayers; ++i) {
        PublicPlayer p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        world->players.push_back(p);
    }
    SharedPtr<const PublicSnapshot> shared = std::move(world);

    std::vector<Request> requests;
    for (PlayerId i = 0; i < kPlayers; ++i) {
        Request r;
        r.id = first_id + static_cast<std::uint64_t>(i);
        r.kind = RequestKind::Speech;
        r.world = shared;
        r.you.self = i;
        requests.push_back(r);
    }
    return requests;
}

}  // namespace

void threads_answer_one_shared_request() {
    std::vector<SharedPtr<Player>> players = seat<Probe>();
    ThreadEngine engine;
    engine.start(players);
    CHECK(engine.running());

    std::vector<Response> answers = engine.exchange(speech_round(100));
    CHECK(answers.size() == kPlayers);
    std::set<std::thread::id> threads;
    for (PlayerId i = 0; i < kPlayers; ++i) {
        const Response& a = answers[static_cast<std::size_t>(i)];
        // Порядок — как у запросов, хотя быстрее всех ответил последний.
        CHECK(a.from == i && a.request_id == 100 + static_cast<std::uint64_t>(i));
        CHECK(a.speech.find("Игрок " + std::to_string(i + 1)) != std::string::npos);
        // Чтение поля игрока после барьера: гонки нет (проверяет TSan).
        auto* probe = static_cast<Probe*>(players[static_cast<std::size_t>(i)].get());
        threads.insert(probe->seen_thread);
        CHECK(probe->seen_thread != std::this_thread::get_id());
    }
    CHECK(threads.size() == kPlayers);  // у каждого игрока своя нить
    engine.stop();
    CHECK(!engine.running());
}

void threads_live_across_phases() {
    std::vector<SharedPtr<Player>> players = seat<Probe>();
    ThreadEngine engine;
    engine.start(players);

    engine.exchange(speech_round(1));
    std::vector<std::thread::id> first;
    for (auto& p : players) {
        first.push_back(static_cast<Probe*>(p.get())->seen_thread);
    }
    engine.exchange(speech_round(7));
    for (std::size_t i = 0; i < players.size(); ++i) {
        auto* probe = static_cast<Probe*>(players[i].get());
        CHECK(probe->seen_thread == first[i]);  // та же нить во второй фазе
        CHECK(probe->calls == 2);
    }

    // Частичная рассылка: отвечают только адресаты.
    std::vector<Request> two = speech_round(20);
    std::vector<Request> subset = {two[1], two[4]};
    std::vector<Response> answers = engine.exchange(subset);
    CHECK(answers.size() == 2 && answers[0].from == 1 && answers[1].from == 4);
}

void exception_reaches_main_thread() {
    std::vector<SharedPtr<Player>> players = seat<Exploder>();
    ThreadEngine engine;
    engine.start(players);

    bool caught = false;
    try {
        engine.exchange(speech_round(1));
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "игрок 3 сломался";
    }
    CHECK(caught);

    // Нить игрока пережила исключение, движок работает дальше.
    std::vector<Request> again = speech_round(10);
    std::vector<Response> ok = engine.exchange({again[0], again[1]});
    CHECK(ok.size() == 2 && ok[0].speech == "Я в порядке");
}

void destructor_joins_during_unwinding() {
    // Исключение покидает область видимости движка: деструктор должен
    // остановить и присоединить нити, иначе std::terminate.
    bool caught = false;
    try {
        ThreadEngine engine;
        engine.start(seat<Exploder>());
        engine.exchange(speech_round(1));
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
}

void bad_requests_rejected() {
    ThreadEngine engine;
    bool not_started = false;
    try {
        engine.exchange(speech_round(1));
    } catch (const std::logic_error&) {
        not_started = true;
    }
    CHECK(not_started);

    engine.start(seat<Probe>());
    std::vector<Request> r = speech_round(1);
    bool duplicate = false;
    try {
        engine.exchange({r[0], r[0]});
    } catch (const std::invalid_argument&) {
        duplicate = true;
    }
    CHECK(duplicate);

    bool stranger = false;
    r[0].you.self = 42;
    try {
        engine.exchange({r[0]});
    } catch (const std::invalid_argument&) {
        stranger = true;
    }
    CHECK(stranger);

    engine.stop();
    engine.stop();  // повторная остановка безвредна
    CHECK(!engine.running());
}

void idle_threads_sleep() {
    // Шесть нитей ждут работы 300 мс. Если бы они крутились в цикле,
    // процессорное время процесса выросло бы на ~1.8 с.
    ThreadEngine engine;
    engine.start(seat<Probe>());
    std::clock_t before = std::clock();
    std::this_thread::sleep_for(300ms);
    std::clock_t spent = std::clock() - before;
    CHECK(static_cast<double>(spent) / CLOCKS_PER_SEC < 0.05);
}

void same_seed_same_answers() {
    auto play = [](std::uint64_t seed) {
        ThreadEngine engine;
        std::vector<SharedPtr<Player>> players;
        for (PlayerId i = 0; i < kPlayers; ++i) {
            players.push_back(PlayerFactory::create(
                kAllRoles[static_cast<std::size_t>(i) % kAllRoles.size()],
                PlayerSetup(i, "Игрок " + std::to_string(i + 1), Rng::stream(seed, static_cast<std::uint64_t>(i) + 1))));
        }
        engine.start(players);
        std::vector<std::string> out;
        for (int round = 0; round < 3; ++round) {
            for (const Response& a : engine.exchange(speech_round(static_cast<std::uint64_t>(round) * 10))) {
                out.push_back(a.speech);
            }
        }
        return out;
    };
    CHECK(play(5) == play(5));  // нити не влияют на исход
    CHECK(play(5) != play(6));
}

int main() {
    std::cout << "ThreadEngine\n";
    test::run("нити отвечают на общий запрос", &threads_answer_one_shared_request);
    test::run("нити живут между фазами", &threads_live_across_phases);
    test::run("исключение доезжает до главной нити", &exception_reaches_main_thread);
    test::run("деструктор присоединяет нити при исключении", &destructor_joins_during_unwinding);
    test::run("неверные вызовы отвергаются", &bad_requests_rejected);
    test::run("ждущие нити не тратят процессор", &idle_threads_sleep);
    test::run("одно зерно — одни ответы", &same_seed_same_answers);
    return test::finish();
}
