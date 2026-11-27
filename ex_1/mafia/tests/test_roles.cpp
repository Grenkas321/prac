// Тесты иерархии ролей: фабрика, ночные ходы каждой роли, тайные сведения,
// дневное поведение, правило доктора, воспроизводимость по зерну.
#include "roles/commissar.hpp"
#include "roles/doctor.hpp"
#include "roles/mafioso.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <set>

using namespace mafia;

namespace {

constexpr std::uint64_t kSeed = 2026;

// Стол из n живых игроков «Игрок 1» .. «Игрок n».
SharedPtr<const PublicSnapshot> table(int n, Phase phase = Phase::Night) {
    auto world = make_shared_ptr<PublicSnapshot>();
    world->day = 1;
    world->phase = phase;
    for (PlayerId i = 0; i < n; ++i) {
        PublicPlayer p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        world->players.push_back(p);
    }
    return world;
}

SharedPtr<Player> make(RoleKind kind, PlayerId id, std::uint64_t seed = kSeed) {
    return PlayerFactory::create(kind, PlayerSetup(id, "Игрок " + std::to_string(id + 1),
                                                   Rng::stream(seed, static_cast<std::uint64_t>(id) + 1)));
}

Request night_request(const Player& p, std::vector<PlayerId> targets, std::uint64_t id = 1) {
    Request r;
    r.id = id;
    r.kind = RequestKind::NightMove;
    r.world = table(6);
    r.you.self = p.id();
    r.you.role = p.role();
    r.targets = std::move(targets);
    for (NightActionKind a : role_capabilities(p.role()).night_actions) {
        r.actions.push_back(a);
    }
    return r;
}

Request day_request(const Player& p, RequestKind kind, std::vector<PlayerId> targets = {}) {
    Request r;
    r.id = 7;
    r.kind = kind;
    r.world = table(6, Phase::Voting);
    r.you.self = p.id();
    r.you.role = p.role();
    r.targets = std::move(targets);
    return r;
}

KnownIdentity fact(PlayerId id, bool is_mafia, std::optional<RoleKind> role = std::nullopt) {
    KnownIdentity k;
    k.id = id;
    k.is_mafia = is_mafia;
    k.role = role;
    return k;
}

bool among(PlayerId x, const std::vector<PlayerId>& v) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

}  // namespace

void factory_creates_every_role() {
    for (RoleKind kind : kAllRoles) {
        SharedPtr<Player> p = make(kind, 2);
        CHECK(p);
        CHECK(p->role() == kind);
        CHECK(p->id() == 2 && p->name() == "Игрок 3");
        // Подключено ровно то, что разрешают концепты.
        CHECK(p->has_night_move() == role_capabilities(kind).night_actor);
        CHECK(p->receives_secrets() == role_capabilities(kind).receives_secrets);
    }
}

void civilian_has_no_night_move() {
    SharedPtr<Player> civ = make(RoleKind::Civilian, 0);
    Response r = civ->handle(night_request(*civ, {1, 2, 3}, 42));
    CHECK(r.night.kind == NightActionKind::None && !r.night.target);
    CHECK(r.request_id == 42 && r.from == 0);  // протокольные поля заполнила база
}

void mafioso_kills_and_spares_allies() {
    SharedPtr<Player> p = make(RoleKind::Mafioso, 0);
    Request req = night_request(*p, {1, 2, 3, 4});
    req.you.known = {fact(0, true, RoleKind::Mafioso), fact(1, true, RoleKind::Mafioso),
                     fact(2, true, RoleKind::Mafioso)};

    for (int i = 0; i < 50; ++i) {  // много раз: выбор случайный
        req.id = static_cast<std::uint64_t>(i);
        Response r = p->handle(req);
        CHECK(r.night.kind == NightActionKind::Kill);
        CHECK(r.night.target && among(*r.night.target, {3, 4}));  // 1 и 2 — союзники
        CHECK(!r.speech.empty());                                 // реплика в приват мафии
    }
    auto* m = static_cast<Mafioso*>(p.get());
    CHECK(m->allies() == std::set<PlayerId>({1, 2}));  // себя в союзники не записал

    Response v = p->handle(day_request(*p, RequestKind::Vote, {1, 2, 5}));
    CHECK(v.target == 5);  // днём тоже против своих не голосует
}

void commissar_checks_then_shoots() {
    SharedPtr<Player> p = make(RoleKind::Commissar, 0);

    // Ничего не знает — проверяет.
    Response first = p->handle(night_request(*p, {1, 2, 3}));
    CHECK(first.night.kind == NightActionKind::Check);
    CHECK(first.night.target && among(*first.night.target, {1, 2, 3}));

    // Ведущий доставил: 1 — мирный. Следующая проверка — не 1.
    Request second = night_request(*p, {1, 2, 3});
    second.you.known = {fact(1, false)};
    Response r2 = p->handle(second);
    CHECK(r2.night.kind == NightActionKind::Check);
    CHECK(r2.night.target && among(*r2.night.target, {2, 3}));

    // Ведущий доставил: 3 — мафия. Стреляем в 3.
    Request third = night_request(*p, {1, 2, 3});
    third.you.known = {fact(1, false), fact(3, true)};
    Response r3 = p->handle(third);
    CHECK(r3.night.kind == NightActionKind::Shoot && r3.night.target == 3);

    // Днём голосует против проверенного мафиози и намекает на него в речи.
    Response v = p->handle(day_request(*p, RequestKind::Vote, {1, 2, 3}));
    CHECK(v.target == 3);
    Response s = p->handle(day_request(*p, RequestKind::Speech));
    CHECK(s.speech.find("Игрок 4") != std::string::npos);

    // Завещание указывает на мафию.
    Response w = p->handle(day_request(*p, RequestKind::LastWord));
    CHECK(w.speech.find("завещание") != std::string::npos && w.speech.find("Игрок 4") != std::string::npos);
}

void doctor_heals_by_rule() {
    SharedPtr<Player> p = make(RoleKind::Doctor, 0);
    auto* doc = static_cast<Doctor*>(p.get());

    // Предлагаем только себя — лечит себя.
    Response n1 = p->handle(night_request(*p, {0}));
    CHECK(n1.night.kind == NightActionKind::Heal && n1.night.target == 0);
    CHECK(doc->healed_self() && doc->last_healed() == 0);

    // Себя больше нельзя, даже если ведущий по ошибке предложил.
    for (int i = 0; i < 30; ++i) {
        Response r = p->handle(night_request(*p, {0, 1, 2}));
        CHECK(r.night.kind == NightActionKind::Heal && r.night.target != 0);
        // И никогда — того же, кого лечил прошлой ночью.
        PlayerId prev = *doc->last_healed();
        Response next = p->handle(night_request(*p, {1, 2}));
        CHECK(next.night.target != prev);
    }
}

void doctor_rule_on_night_log() {
    GameState st;
    const PlayerId doc = 1;
    auto heal = [&](int night, PlayerId target) {
        NightRecord r;
        r.night = night;
        r.actor = doc;
        r.action.kind = NightActionKind::Heal;
        r.action.target = target;
        st.night_log.push_back(r);
    };

    st.day = 1;
    CHECK(Doctor::may_protect(st, doc, doc));  // первая ночь: можно всё
    heal(1, 4);
    st.day = 2;
    CHECK(!Doctor::may_protect(st, doc, 4));  // 4 — прошлой ночью
    CHECK(Doctor::may_protect(st, doc, doc));
    heal(2, doc);
    st.day = 3;
    CHECK(Doctor::may_protect(st, doc, 4));    // через ночь — снова можно
    CHECK(!Doctor::may_protect(st, doc, doc)); // себя — уже было
    heal(3, 4);
    st.day = 5;
    CHECK(!Doctor::may_protect(st, doc, doc));  // и через сколько угодно ночей — нет
    CHECK(Doctor::may_protect(st, 2, 4));       // чужой журнал не мешает другому доктору
}

void maniac_kills_from_list() {
    SharedPtr<Player> p = make(RoleKind::Maniac, 5);
    Response r = p->handle(night_request(*p, {0, 1, 2}));
    CHECK(r.night.kind == NightActionKind::Kill);
    CHECK(r.night.target && among(*r.night.target, {0, 1, 2}));
    CHECK(!p->receives_secrets());
}

void day_behaviour_common() {
    for (RoleKind kind : kAllRoles) {
        SharedPtr<Player> p = make(kind, 0);
        Response v = p->handle(day_request(*p, RequestKind::Vote, {1, 2, 3}));
        CHECK(v.target && among(*v.target, {1, 2, 3}));
        Response s = p->handle(day_request(*p, RequestKind::Speech));
        CHECK(!s.speech.empty());
        // Реплика не раскрывает роль говорящего.
        if (kind != RoleKind::Civilian) {
            CHECK(s.speech.find(role_name(kind)) == std::string::npos);
        }
        Request choice = day_request(*p, RequestKind::Choice);
        choice.options = {"да", "нет"};
        Response c = p->handle(choice);
        CHECK(c.choice && *c.choice < 2);
        Response stop = p->handle(day_request(*p, RequestKind::Stop));
        CHECK(!stop.target && stop.speech.empty());
    }
}

void same_seed_same_moves() {
    auto run = [](std::uint64_t seed) {
        std::vector<std::optional<PlayerId>> out;
        for (RoleKind kind : kAllRoles) {
            SharedPtr<Player> p = make(kind, 0, seed);
            for (int i = 0; i < 5; ++i) {
                out.push_back(p->handle(night_request(*p, {1, 2, 3, 4, 5})).night.target);
                out.push_back(p->handle(day_request(*p, RequestKind::Vote, {1, 2, 3, 4, 5})).target);
            }
        }
        return out;
    };
    CHECK(run(1) == run(1));
    CHECK(run(1) != run(2));
}

int main() {
    std::cout << "Иерархия ролей\n";
    test::run("фабрика: все роли, способности по концептам", &factory_creates_every_role);
    test::run("мирный: ночного хода нет", &civilian_has_no_night_move);
    test::run("мафиози: убивает, союзников не трогает", &mafioso_kills_and_spares_allies);
    test::run("комиссар: проверка, затем выстрел", &commissar_checks_then_shoots);
    test::run("доктор: лечит по своей памяти", &doctor_heals_by_rule);
    test::run("доктор: правило по тайному журналу", &doctor_rule_on_night_log);
    test::run("маньяк: убивает из списка", &maniac_kills_from_list);
    test::run("дневное поведение всех ролей", &day_behaviour_common);
    test::run("одно зерно — одни и те же ходы", &same_seed_same_moves);
    return test::finish();
}
