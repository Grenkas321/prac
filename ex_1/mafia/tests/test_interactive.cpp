// Тесты режима --interactive: пользователь — один из игроков.
//
//   * разбор ввода для каждого вида запроса: допустимое, недопустимое
//     (переспросить), пропуск хода, конец ввода;
//   * движок-обёртка: ответы в порядке запросов, остальные — ботам, Stop — боту;
//   * целые партии со скриптованным вводом на обоих движках: доигрываются,
//     каждое обращение к пользователю — с подсказкой ведущего;
//   * сразу закрытый ввод = обычная партия (за пользователя играет бот);
//   * место пользователя не отдаётся ИИ, его роль видит только он.
#include "engine/console_engine.hpp"
#include "engine/coro_engine.hpp"
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <sstream>
#include <string>

using namespace mafia;

namespace {

SharedPtr<const PublicSnapshot> world() {
    auto w = make_shared_ptr<PublicSnapshot>();
    w->day = 2;
    w->phase = Phase::Voting;
    for (PlayerId i = 0; i < 6; ++i) {
        PublicPlayer p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        p.alive = i != 5;
        w->players.push_back(p);
    }
    return w;
}

Request request(RequestKind kind) {
    Request r;
    r.id = 11;
    r.kind = kind;
    r.world = world();
    r.you.self = 0;
    r.you.role = RoleKind::Civilian;
    return r;
}

// Ответ на запрос при данном вводе; out — что увидел пользователь.
std::optional<Response> answer(const Request& r, const std::string& input, std::string* seen = nullptr) {
    std::istringstream in(input);
    std::ostringstream out;
    std::optional<Response> a = ConsoleEngine::ask(r, in, out);
    if (seen) {
        *seen = out.str();
    }
    return a;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int count(const std::string& text, const std::string& part) {
    int n = 0;
    for (auto pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + part.size())) {
        ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Разбор ввода
// ---------------------------------------------------------------------------

void vote_input() {
    Request r = request(RequestKind::Vote);
    r.targets = {1, 2, 3};
    std::string seen;
    // Мусор, мёртвый, сам, чужой номер — переспросить; затем допустимый.
    const auto a = answer(r, "abc\n6\n1\n5\n3\n", &seen);
    CHECK(a && a->target == 2);
    CHECK(count(seen, "Не понял") == 4);
    CHECK(has(seen, "Номера: 2, 3, 4"));           // подсказка: какие номера можно
    CHECK(has(seen, "фаза: Дневное голосование"));  // подсказка: какая фаза
    CHECK(!has(seen, "воздержаться"));              // мирным пропускать нельзя
    r.can_abstain = true;
    const auto skip = answer(r, "0\n", &seen);
    CHECK(skip && !skip->target && has(seen, "0 — воздержаться"));
    CHECK(!answer(r, "abc\n"));  // ввод кончился раньше, чем пришёл ответ
    CHECK(!answer(r, ""));
}

void speech_input() {
    const Request r = request(RequestKind::Speech);
    const auto plain = answer(r, "Всем привет\n");
    CHECK(plain && plain->speech == "Всем привет" && !plain->claim);
    const auto accuse = answer(r, "обвинить 4 врёт про голос\n");
    CHECK(accuse && accuse->claim && accuse->claim->about == 3 && accuse->claim->mafia);
    CHECK(accuse->speech == "врёт про голос");
    const auto defend = answer(r, "Защитить 2\n");
    CHECK(defend && defend->claim && defend->claim->about == 1 && !defend->claim->mafia);
    CHECK(defend->speech == "Игрок 2 — мирный.");  // без текста — утверждение словами
    std::string seen;
    // Себя, выбывшего и не номер обвинить нельзя — переспросить.
    const auto again = answer(r, "обвинить 1\nобвинить 6\nобвинить кого-то\nобвинить 5\n", &seen);
    CHECK(again && again->claim && again->claim->about == 4 && count(seen, "Не понял") == 3);
    const auto silent = answer(r, "\n");
    CHECK(silent && silent->speech.empty());
    const auto windows = answer(r, "привет\r\n");  // перевод строки из Windows
    CHECK(windows && windows->speech == "привет");
}

void last_word_and_choice() {
    const auto lw = answer(request(RequestKind::LastWord), "Прощайте\n");
    CHECK(lw && lw->speech == "Прощайте");

    Request c = request(RequestKind::Choice);
    c.choice = ChoiceKind::KickAll;
    c.targets = {1, 2};
    c.options = {"да", "нет"};
    std::string seen;
    const auto a = answer(c, "0\n3\n2\n", &seen);
    CHECK(a && a->choice == std::size_t{1});
    CHECK(has(seen, "Выгнать всех разом") && has(seen, "1 — да") && count(seen, "Не понял") == 2);
}

void night_input() {
    // Комиссар: выбор действия, затем цель.
    Request r = request(RequestKind::NightMove);
    r.you.role = RoleKind::Commissar;
    r.actions = {NightActionKind::Check, NightActionKind::Shoot};
    r.targets = {1, 2, 3};
    std::string seen;
    const auto a = answer(r, "3\n2\n9\n4\n", &seen);
    CHECK(a && a->night.kind == NightActionKind::Shoot && a->night.target == 3);
    CHECK(has(seen, "1 — проверка") && has(seen, "2 — выстрел") && !has(seen, "пропустить"));
    CHECK(count(seen, "Не понял") == 2);

    // Совет мафии — своя подсказка; пропускать нельзя.
    Request m = request(RequestKind::NightMove);
    m.you.role = RoleKind::Mafioso;
    m.you.known = {KnownIdentity{4, true, RoleKind::Mafioso}};
    m.actions = {NightActionKind::Kill};
    m.targets = {1, 2};
    const auto k = answer(m, "3\n", &seen);
    CHECK(k && k->night.kind == NightActionKind::Kill && k->night.target == 2);
    CHECK(has(seen, "Совет мафии") && has(seen, "Вы знаете: Игрок 5 — Мафиози"));  // свои сведения

    // Необязательный ход — 0 пропускает.
    Request d = request(RequestKind::NightMove);
    d.you.role = RoleKind::Drunk;
    d.actions = {NightActionKind::Visit};
    d.targets = {1, 2};
    d.can_abstain = true;
    const auto skip = answer(d, "0\n");
    CHECK(skip && skip->night.kind == NightActionKind::None);

    // Журналист — второй игрок; похититель — роль.
    Request j = request(RequestKind::NightMove);
    j.actions = {NightActionKind::Compare};
    j.targets = {1, 2, 3};
    const auto cmp = answer(j, "2\n2\n4\n");
    CHECK(cmp && cmp->night.target == 1 && cmp->night.second == 3);
    Request kd = request(RequestKind::NightMove);
    kd.actions = {NightActionKind::Kidnap};
    kd.targets = {1, 2};
    kd.can_abstain = true;
    const auto kid = answer(kd, "2\nПрезидент\nКомиссар\n");
    CHECK(kid && kid->night.target == 1 && kid->night.guess == RoleKind::Commissar);
    CHECK(!answer(kd, "2\n"));  // роль так и не названа — ввод кончился
}

// ---------------------------------------------------------------------------
// Движок-обёртка
// ---------------------------------------------------------------------------

namespace {

std::vector<SharedPtr<Player>> seat(int n) {
    std::vector<SharedPtr<Player>> players;
    for (PlayerId i = 0; i < n; ++i) {
        players.push_back(PlayerFactory::create(RoleKind::Civilian, PlayerSetup(i, "Игрок " + std::to_string(i + 1), Rng(i))));
    }
    return players;
}

}  // namespace

void wrapper_keeps_order() {
    for (bool coro : {false, true}) {
        SharedPtr<Engine> inner;
        if (coro) {
            inner = make_shared_ptr<CoroEngine>();
        } else {
            inner = make_shared_ptr<ThreadEngine>();
        }
        std::istringstream in("Это я говорю\n");
        std::ostringstream out;
        ConsoleEngine engine(inner, 2, in, out);
        engine.start(seat(4));
        CHECK(has(std::string(engine.name()), "пользователь — Игрок 3"));
        std::vector<Request> batch;
        for (PlayerId p = 0; p < 4; ++p) {
            Request r = request(RequestKind::Speech);
            r.id = 100 + static_cast<std::uint64_t>(p);
            r.you.self = p;
            batch.push_back(r);
        }
        const std::vector<Response> answers = engine.exchange(batch);
        CHECK(answers.size() == 4);
        bool order = true;
        for (std::size_t i = 0; i < answers.size(); ++i) {
            order = order && answers[i].from == static_cast<PlayerId>(i) && answers[i].request_id == 100 + i;
        }
        CHECK(order);
        CHECK(answers[2].speech == "Это я говорю");  // место пользователя — из консоли
        CHECK(count(out.str(), ">>> Ведущий — вам") == 1);  // остальных не спрашивали
        // Ввод закончился — дальше отвечает бот, без вопросов.
        const std::vector<Response> next = engine.exchange(batch);
        CHECK(next.size() == 4 && engine.input_closed() && !next[2].speech.empty());
        CHECK(has(out.str(), "дальше за вас решает бот"));
        engine.stop();
    }
}

// ---------------------------------------------------------------------------
// Целые партии
// ---------------------------------------------------------------------------

namespace {

struct Game {
    std::string console;
    GameState state;
    bool closed = false;
};

Game play(std::uint64_t seed, PlayerId human, const std::string& input, bool coro, bool open = false,
          int ai = 0) {
    HostConfig c;
    c.players = 8;
    c.seed = seed;
    c.open_announcements = open;
    c.human_seat = human;
    if (ai > 0) {
        c.ai_players = ai;
        // Советник, который всегда молчит, — нужно только распределение мест.
        struct Silent final : Advisor {
            std::optional<AdvisorAnswer> advise(const Request&) override { return std::nullopt; }
        };
        c.make_advisor = [](PlayerId, int) -> SharedPtr<Advisor> { return make_shared_ptr<Silent>(); };
    }
    SharedPtr<Engine> inner;
    if (coro) {
        inner = make_shared_ptr<CoroEngine>();
    } else {
        inner = make_shared_ptr<ThreadEngine>();
    }
    std::istringstream in(input);
    std::ostringstream out;
    auto engine = make_shared_ptr<ConsoleEngine>(inner, human, in, out);
    Host host(c, engine, out);
    host.setup();
    host.play();
    host.shutdown();
    return Game{out.str(), host.state(), engine->input_closed()};
}

// Ввод «как у человека, который не знает правил»: числа по кругу и фразы.
// Любой запрос рано или поздно получает допустимый ответ.
std::string chaotic_input() {
    std::string s;
    for (int i = 0; i < 4000; ++i) {
        s += (i % 9 == 8 ? std::string("обвинить ") + std::to_string(i % 8 + 1) : std::to_string(i % 8 + 1)) + "\n";
    }
    return s;
}

std::string without(const std::string& text, std::initializer_list<const char*> prefixes) {
    std::istringstream in(text);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        bool drop = line.empty();  // пустые строки-разделители вокруг подсказок
        for (const char* p : prefixes) {
            drop = drop || line.starts_with(p);
        }
        if (!drop) {
            out += line + "\n";
        }
    }
    return out;
}

}  // namespace

void full_games_with_user() {
    for (bool coro : {false, true}) {
        for (std::uint64_t seed : {1, 2, 3}) {
            const Game g = play(seed, static_cast<PlayerId>(seed % 8), chaotic_input(), coro, seed == 2);
            CHECK(g.state.finished);
            CHECK(has(g.console, "RESULT "));
            CHECK(has(g.console, ">>> Ведущий — вам"));
            CHECK(has(g.console, "*** Вы — Игрок " + std::to_string(seed % 8 + 1) + ". Ваша роль:"));
            CHECK(!g.closed);  // ввода хватило на всю партию: всё решал «человек»
        }
    }
}

void immediate_eof_is_plain_game() {
    // Ввода нет вовсе: за пользователя с первого же вопроса играет бот его
    // роли, и партия совпадает с обычной партией с тем же зерном.
    for (bool coro : {false, true}) {
        const Game user = play(42, 4, "", coro);
        HostConfig c;
        c.players = 8;
        c.seed = 42;
        std::ostringstream plain;
        SharedPtr<Engine> engine;
        if (coro) {
            engine = make_shared_ptr<CoroEngine>();
        } else {
            engine = make_shared_ptr<ThreadEngine>();
        }
        Host host(c, engine, plain);
        host.setup();
        host.play();
        host.shutdown();
        CHECK(user.closed);
        const auto noise = {"Исполнение:", "Пользователь играет за", "***", "    ", ">>>", "Ваша реплика",
                            "  обвинить", "  защитить", "Просто текст", "> ", "(ввод закончился"};
        CHECK(without(user.console, noise) == without(plain.str(), noise));
        CHECK(count(without(plain.str(), noise), "\n") > 30);  // сравнение не пустое
        CHECK(user.state.players.size() == host.state().players.size());
    }
}

void user_seat_is_never_ai_and_role_is_private() {
    const Game g = play(7, 3, "", false, false, /*ai=*/7);  // все остальные места — ИИ
    const auto ai_line = g.console.substr(g.console.find("ИИ-агенты:"));
    CHECK(!has(ai_line.substr(0, ai_line.find('\n')), "Игрок 4"));
    CHECK(count(ai_line.substr(0, ai_line.find('\n')), "Игрок") == 7);
    // Роль пользователя сообщается ему, но не попадает в публичную историю.
    bool leaked = false;
    for (const HistoryEntry& h : g.state.history) {
        if (const auto* a = std::get_if<AnnouncementEvent>(&h.event)) {
            leaked = leaked || has(a->text, "Ваша роль");
        }
    }
    CHECK(!leaked);
}

int main() {
    test::run("ввод: голос", &vote_input);
    test::run("ввод: речь и утверждения", &speech_input);
    test::run("ввод: последнее слово и выбор", &last_word_and_choice);
    test::run("ввод: ночные ходы", &night_input);
    test::run("обёртка: порядок ответов, бот после конца ввода", &wrapper_keeps_order);
    test::run("партии с пользователем на обоих движках", &full_games_with_user);
    test::run("ввода нет — обычная партия", &immediate_eof_is_plain_game);
    test::run("место пользователя не ИИ, роль — только ему", &user_seat_is_never_ai_and_role_is_private);
    return test::finish();
}
