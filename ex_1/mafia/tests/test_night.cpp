// Тесты ночной фазы: правила (чистые функции) и сценарии на настоящих нитях,
// где ночные ходы и дневные голоса заданы заранее.
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "game/night.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>

using namespace mafia;

namespace {

// ---------------------------------------------------------------------------
// Сценарий: что делает каждый игрок ночью и против кого голосует днём.
// Заполняется главной нитью ДО exchange, читается нитями игроков внутри
// exchange — «происходит раньше» обеспечивает мьютекс почтового ящика.
// ---------------------------------------------------------------------------
struct Script {
    // Пишет только главная нить (до exchange), нити игроков только читают.
    std::map<std::pair<int, PlayerId>, NightAction> night;  // (ночь, игрок) -> ход
    std::optional<PlayerId> scapegoat;                      // против кого голосуют все

    // Что пришло игроку в ночном запросе. Сюда пишут НЕСКОЛЬКО нитей сразу:
    // ночью все ночные роли отвечают одновременно — поэтому под мьютексом.
    // (Без него ThreadSanitizer находит гонку на вставке в std::map.)
    std::mutex seen_mutex;
    std::map<std::pair<int, PlayerId>, std::vector<PlayerId>> seen_targets;

    void reset() {
        night.clear();
        scapegoat.reset();
        std::lock_guard lock(seen_mutex);
        seen_targets.clear();
    }
    void remember(std::pair<int, PlayerId> key, const std::vector<PlayerId>& targets) {
        std::lock_guard lock(seen_mutex);
        seen_targets[key] = targets;
    }
    std::vector<PlayerId> seen(std::pair<int, PlayerId> key) {
        std::lock_guard lock(seen_mutex);
        return seen_targets.at(key);
    }
};
Script g_script;

template <class Base>
class ScriptedNight : public Base {
public:
    explicit ScriptedNight(PlayerSetup setup) : Base(std::move(setup)) {}

    // Скрывает Base::act — фабрика регистрирует именно этот метод.
    Response act(const Request& request) {
        const std::pair<int, PlayerId> key{request.world->day, this->id()};
        g_script.remember(key, request.targets);
        if (auto it = g_script.night.find(key); it != g_script.night.end()) {
            Response r;
            r.night = it->second;
            return r;
        }
        return Base::act(request);
    }

protected:
    int target_score(const Request& request, Player::Purpose purpose, PlayerId candidate) const override {
        if (purpose == Player::Purpose::Vote && g_script.scapegoat == candidate) {
            return 1000;
        }
        return Base::target_score(request, purpose, candidate);
    }
};

// У мирного нет act(): только голос по сценарию.
class ScriptedCivilian : public Civilian {
public:
    explicit ScriptedCivilian(PlayerSetup setup) : Civilian(std::move(setup)) {}

protected:
    int target_score(const Request& request, Purpose purpose, PlayerId candidate) const override {
        if (purpose == Purpose::Vote && g_script.scapegoat == candidate) {
            return 1000;
        }
        return Civilian::target_score(request, purpose, candidate);
    }
};

SharedPtr<Player> make_scripted(RoleKind role, PlayerSetup setup) {
    switch (role) {
        case RoleKind::Civilian:
            return PlayerFactory::create_as<ScriptedCivilian>(std::move(setup));
        case RoleKind::Mafioso:
            return PlayerFactory::create_as<ScriptedNight<Mafioso>>(std::move(setup));
        case RoleKind::Commissar:
            return PlayerFactory::create_as<ScriptedNight<Commissar>>(std::move(setup));
        case RoleKind::Doctor:
            return PlayerFactory::create_as<ScriptedNight<Doctor>>(std::move(setup));
        case RoleKind::Maniac:
            return PlayerFactory::create_as<ScriptedNight<Maniac>>(std::move(setup));
        default:
            // Тесты этой ночи — про обязательные роли; доп. ролям сценарий не нужен.
            return PlayerFactory::create(role, std::move(setup));
    }
}

// Партия со сценарием: N игроков, зерно фиксированное; роли — из раздачи.
struct Table {
    explicit Table(int players = 8) {
        g_script.reset();
        HostConfig c;
        c.players = players;
        c.seed = 77;
        c.make_player = &make_scripted;
        host = std::make_unique<Host>(c, make_shared_ptr<ThreadEngine>(), out);
        host->setup();
        for (const PlayerState& p : host->state().players) {
            by_role[p.role].push_back(p.id);
        }
    }
    ~Table() { host->shutdown(); }

    PlayerId one(RoleKind r) const { return by_role.at(r).front(); }
    const std::vector<PlayerId>& all(RoleKind r) const { return by_role.at(r); }
    void order(int night, PlayerId who, NightActionKind kind, PlayerId target) {
        NightAction a;
        a.kind = kind;
        a.target = target;
        g_script.night[{night, who}] = a;
    }
    bool alive(PlayerId id) const { return host->state().players[static_cast<std::size_t>(id)].alive; }
    int deaths_on_day(int day) const {
        int n = 0;
        for (const HistoryEntry& h : host->state().history) {
            if (const auto* d = std::get_if<DeathEvent>(&h.event); d && h.day == day && d->cause == DeathCause::Night) {
                ++n;
            }
        }
        return n;
    }

    std::ostringstream out;
    std::unique_ptr<Host> host;
    std::map<RoleKind, std::vector<PlayerId>> by_role;
};

}  // namespace

// ---------------------------------------------------------------------------
// Правила — чистые функции
// ---------------------------------------------------------------------------

void resolve_rules() {
    // Лечение спасает от любого источника.
    NightOutcome a = resolve_night({{3, RoleKind::Mafioso}, {3, RoleKind::Maniac}, {3, RoleKind::Commissar}}, 3);
    CHECK(a.deaths.empty() && a.saved == std::vector<PlayerId>({3}));
    // Несколько источников по одному — одна смерть.
    NightOutcome b = resolve_night({{3, RoleKind::Mafioso}, {3, RoleKind::Maniac}}, std::nullopt);
    CHECK(b.deaths == std::vector<PlayerId>({3}));
    // Разные цели — все гибнут; лечение чужого не спасает.
    NightOutcome c = resolve_night({{5, RoleKind::Maniac}, {1, RoleKind::Mafioso}, {2, RoleKind::Commissar}}, 7);
    CHECK(c.deaths == std::vector<PlayerId>({1, 2, 5}) && c.saved.empty());
    // Порядок нападений не важен.
    NightOutcome d = resolve_night({{1, RoleKind::Mafioso}, {5, RoleKind::Maniac}, {2, RoleKind::Commissar}}, 7);
    CHECK(d.deaths == c.deaths);
}

void mafia_rules() {
    // Большинство.
    CHECK(decide_mafia({{0, 4}, {1, 4}, {2, 5}}, 0).victim == 4);
    // Равенство: решает Босс своим голосом.
    CHECK(decide_mafia({{0, 4}, {1, 5}}, 0).victim == 4);
    CHECK(decide_mafia({{0, 4}, {1, 5}}, 1).victim == 5);
    // Равенство, а Босс голосовал не за лидера: ведущий спросит Босса отдельно.
    MafiaDecision t = decide_mafia({{0, 6}, {1, 4}, {2, 4}, {3, 5}, {4, 5}}, 0);
    CHECK(!t.victim && t.tied == std::vector<PlayerId>({4, 5}));
    // Мафиози остался один — его голос и есть решение.
    CHECK(decide_mafia({{3, 1}}, 3).victim == 1);
    CHECK(!decide_mafia({}, 0).victim);
}

// ---------------------------------------------------------------------------
// Сценарии на нитях
// ---------------------------------------------------------------------------

void doctor_saves_victim() {
    Table t;
    const PlayerId victim = t.all(RoleKind::Civilian).front();
    t.host->run_day();  // день 1: без голосования
    for (PlayerId m : t.all(RoleKind::Mafioso)) {
        t.order(1, m, NightActionKind::Kill, victim);
    }
    t.order(1, t.one(RoleKind::Maniac), NightActionKind::Kill, victim);  // и маньяк туда же
    t.order(1, t.one(RoleKind::Doctor), NightActionKind::Heal, victim);
    t.order(1, t.one(RoleKind::Commissar), NightActionKind::Check, t.one(RoleKind::Maniac));
    t.host->run_night();

    CHECK(t.host->pending_deaths().empty());
    g_script.scapegoat = t.all(RoleKind::Civilian).back();  // днём казнят другого, не спасённого
    t.host->run_day();
    CHECK(t.alive(victim));
    CHECK(t.deaths_on_day(2) == 0);
    CHECK(t.out.str().find("этой ночью никто не погиб") != std::string::npos);
    // Закрытое объявление: о спасении никто не узнаёт.
    CHECK(t.out.str().find("спас") == std::string::npos);
}

void doctor_cannot_repeat() {
    Table t;
    const std::vector<PlayerId>& civ = t.all(RoleKind::Civilian);
    const PlayerId patient = civ[0];
    const PlayerId doctor = t.one(RoleKind::Doctor);

    t.host->run_day();
    // Ночь 1 целиком по сценарию: все бьют пациента, доктор его спасает.
    // Иначе боты могли бы убить «козла отпущения» дня 2, и голосование
    // стало бы случайным.
    for (PlayerId m : t.all(RoleKind::Mafioso)) {
        t.order(1, m, NightActionKind::Kill, patient);
    }
    t.order(1, t.one(RoleKind::Maniac), NightActionKind::Kill, patient);
    t.order(1, t.one(RoleKind::Commissar), NightActionKind::Check, civ[2]);
    t.order(1, doctor, NightActionKind::Heal, patient);
    t.host->run_night();
    CHECK(t.host->pending_deaths().empty());

    g_script.scapegoat = civ[1];  // день 2: казнят другого мирного, без случайностей
    t.host->run_day();
    CHECK(!t.alive(civ[1]));

    // Ночь 2: доктор пытается лечить того же — ведущий не предлагает и не принимает.
    for (PlayerId m : t.all(RoleKind::Mafioso)) {
        t.order(2, m, NightActionKind::Kill, patient);
    }
    t.order(2, doctor, NightActionKind::Heal, patient);
    t.host->run_night();

    const std::vector<PlayerId> offered = g_script.seen({2, doctor});
    CHECK(std::find(offered.begin(), offered.end(), patient) == offered.end());
    CHECK(std::find(offered.begin(), offered.end(), doctor) != offered.end());  // себя — ещё можно
    CHECK(t.out.str().find("недопустимый ответ от " + t.host->state().players[static_cast<std::size_t>(doctor)].name) !=
          std::string::npos);
    // Лечение не засчитано — пациент погибает.
    const auto& pending = t.host->pending_deaths();
    CHECK(std::find(pending.begin(), pending.end(), patient) != pending.end());
    for (const NightRecord& r : t.host->state().night_log) {
        CHECK(!(r.night == 2 && r.actor == doctor && r.action.kind == NightActionKind::Heal));
    }
}

void doctor_self_heal_once() {
    // Три ночи целиком по сценарию — доктор гарантированно доживает до ночи 3.
    Table t;
    const PlayerId doctor = t.one(RoleKind::Doctor);
    const PlayerId com = t.one(RoleKind::Commissar);
    const PlayerId maniac = t.one(RoleKind::Maniac);
    const std::vector<PlayerId>& civ = t.all(RoleKind::Civilian);  // при N = 8 их трое
    auto everyone_kills = [&](int night, PlayerId target) {
        for (PlayerId m : t.all(RoleKind::Mafioso)) {
            t.order(night, m, NightActionKind::Kill, target);
        }
        t.order(night, maniac, NightActionKind::Kill, target);
    };

    t.host->run_day();
    everyone_kills(1, civ[0]);
    t.order(1, com, NightActionKind::Check, civ[1]);
    t.order(1, doctor, NightActionKind::Heal, doctor);  // себя — первый и последний раз
    t.host->run_night();

    g_script.scapegoat = civ[1];
    t.host->run_day();
    everyone_kills(2, civ[2]);
    t.order(2, com, NightActionKind::Check, maniac);
    t.order(2, doctor, NightActionKind::Heal, civ[2]);
    t.host->run_night();
    const std::vector<PlayerId> night2 = g_script.seen({2, doctor});
    CHECK(std::find(night2.begin(), night2.end(), doctor) == night2.end());

    g_script.scapegoat = maniac;
    t.host->run_day();
    CHECK(t.alive(doctor));
    t.host->run_night();  // ночь 3: прошлой ночью лечил civ[2], себя — уже было
    const std::vector<PlayerId> night3 = g_script.seen({3, doctor});
    CHECK(std::find(night3.begin(), night3.end(), doctor) == night3.end());   // себя — никогда больше
    CHECK(std::find(night3.begin(), night3.end(), civ[2]) == night3.end());   // не две ночи подряд
    CHECK(!night3.empty());
}

void commissar_check_is_private() {
    Table t;
    const PlayerId com = t.one(RoleKind::Commissar);
    const PlayerId maf = t.all(RoleKind::Mafioso).front();
    const PlayerId maniac = t.one(RoleKind::Maniac);
    const PlayerId nobody = t.all(RoleKind::Civilian).back();  // в него бьют все — ночь не трогает проверяемых
    t.host->run_day();
    t.order(1, com, NightActionKind::Check, maf);
    for (PlayerId m : t.all(RoleKind::Mafioso)) {
        t.order(1, m, NightActionKind::Kill, nobody);
    }
    t.order(1, maniac, NightActionKind::Kill, nobody);
    t.host->run_night();

    // Результат — в сводке комиссара: «мафия».
    Briefing b = t.host->briefing_for(com);
    auto it = std::find_if(b.known.begin(), b.known.end(), [&](const KnownIdentity& k) { return k.id == maf; });
    CHECK(it != b.known.end() && it->is_mafia);

    // Больше никто его не получает.
    for (const PlayerState& p : t.host->state().players) {
        if (p.id == com) {
            continue;
        }
        for (const KnownIdentity& k : t.host->briefing_for(p.id).known) {
            CHECK(p.role == RoleKind::Mafioso);  // мафия знает только своих
            CHECK(k.is_mafia);
        }
    }
    // В публичной истории ни следа проверки и ночных ходов.
    for (const HistoryEntry& h : t.host->state().history) {
        CHECK(h.phase != Phase::Night);
    }

    // Ночь 2: проверка маньяка показывает «не мафия».
    g_script.scapegoat = t.all(RoleKind::Civilian).front();
    t.host->run_day();
    t.order(2, com, NightActionKind::Check, maniac);
    t.host->run_night();
    Briefing b2 = t.host->briefing_for(com);
    auto m = std::find_if(b2.known.begin(), b2.known.end(), [&](const KnownIdentity& k) { return k.id == maniac; });
    CHECK(m != b2.known.end() && !m->is_mafia);
    CHECK(b2.known.size() == 2);  // обе проверки
}

void simultaneous_deaths() {
    Table t;
    const std::vector<PlayerId>& civ = t.all(RoleKind::Civilian);
    const PlayerId maniac = t.one(RoleKind::Maniac);
    const PlayerId maf = t.all(RoleKind::Mafioso).front();
    t.host->run_day();
    // Мафия -> маньяка, маньяк -> мафиози, комиссар стреляет в мирного, доктор лечит третьего.
    for (PlayerId m : t.all(RoleKind::Mafioso)) {
        t.order(1, m, NightActionKind::Kill, maniac);
    }
    t.order(1, maniac, NightActionKind::Kill, maf);
    t.order(1, t.one(RoleKind::Commissar), NightActionKind::Shoot, civ[0]);
    t.order(1, t.one(RoleKind::Doctor), NightActionKind::Heal, civ[1]);
    t.host->run_night();

    // Ночью никто не умер «по-настоящему» — только утром.
    CHECK(t.alive(maniac) && t.alive(maf) && t.alive(civ[0]));
    std::vector<PlayerId> expected = {maniac, maf, civ[0]};
    std::sort(expected.begin(), expected.end());
    CHECK(t.host->pending_deaths() == expected);

    g_script.scapegoat = civ[2];  // днём казнят третьего мирного — не того, кого проверяем
    t.host->run_day();
    CHECK(!t.alive(maniac) && !t.alive(maf) && !t.alive(civ[0]) && t.alive(civ[1]));
    CHECK(t.deaths_on_day(2) == 3);
    // У каждого погибшего ночью — последнее слово утром (днём ещё может
    // быть казнь со своим последним словом, её не считаем).
    int last_words = 0;
    for (const HistoryEntry& h : t.host->state().history) {
        if (std::holds_alternative<LastWordEvent>(h.event) && h.day == 2 && h.phase == Phase::Morning) {
            ++last_words;
        }
    }
    CHECK(last_words == 3);
}

void mafia_tie_boss_decides() {
    Table t;  // 8 игроков -> 2 мафиози
    const std::vector<PlayerId>& maf = t.all(RoleKind::Mafioso);
    CHECK(maf.size() == 2);
    const std::vector<PlayerId>& civ = t.all(RoleKind::Civilian);
    t.host->run_day();
    t.order(1, maf[0], NightActionKind::Kill, civ[0]);  // maf[0] — Босс (меньший номер)
    t.order(1, maf[1], NightActionKind::Kill, civ[1]);
    t.order(1, t.one(RoleKind::Maniac), NightActionKind::Kill, maf[1]);  // маньяк не мешает выбору
    t.order(1, t.one(RoleKind::Doctor), NightActionKind::Heal, t.one(RoleKind::Doctor));
    t.host->run_night();
    const auto& pending = t.host->pending_deaths();
    CHECK(std::find(pending.begin(), pending.end(), civ[0]) != pending.end());
    CHECK(std::find(pending.begin(), pending.end(), civ[1]) == pending.end());
}

void lone_mafioso_decides() {
    Table t(5);  // 5 игроков -> 1 мафиози
    const PlayerId maf = t.one(RoleKind::Mafioso);
    const PlayerId civ = t.one(RoleKind::Civilian);
    t.host->run_day();
    t.order(1, maf, NightActionKind::Kill, civ);
    t.order(1, t.one(RoleKind::Maniac), NightActionKind::Kill, maf);
    t.order(1, t.one(RoleKind::Doctor), NightActionKind::Heal, t.one(RoleKind::Commissar));
    t.order(1, t.one(RoleKind::Commissar), NightActionKind::Check, maf);
    t.host->run_night();
    CHECK(t.host->pending_deaths() == std::vector<PlayerId>({std::min(maf, civ), std::max(maf, civ)}));
}

void mafia_never_targets_allies() {
    Table t(9);  // 3 мафиози
    t.host->run_day();
    t.host->run_night();  // ходы — как решат боты
    for (PlayerId m : t.all(RoleKind::Mafioso)) {
        for (PlayerId target : g_script.seen({1, m})) {
            CHECK(t.host->state().players[static_cast<std::size_t>(target)].role != RoleKind::Mafioso);
        }
    }
    // Мирным ночной запрос не приходил: в журнале только ночные роли.
    for (const NightRecord& r : t.host->state().night_log) {
        CHECK(t.host->state().players[static_cast<std::size_t>(r.actor)].role != RoleKind::Civilian);
    }
}

int main() {
    std::cout << "Ночная фаза\n";
    test::run("разрешение ночи: лечение, несколько источников", &resolve_rules);
    test::run("решение мафии: большинство, Босс, одиночка", &mafia_rules);
    test::run("доктор спасает жертву (от мафии и маньяка)", &doctor_saves_victim);
    test::run("доктор не лечит одного две ночи подряд", &doctor_cannot_repeat);
    test::run("доктор лечит себя только один раз", &doctor_self_heal_once);
    test::run("проверка комиссара доходит только до него", &commissar_check_is_private);
    test::run("одновременная гибель от нескольких источников", &simultaneous_deaths);
    test::run("равенство в мафии — решает Босс", &mafia_tie_boss_decides);
    test::run("единственный мафиози решает сам", &lone_mafioso_decides);
    test::run("мафии не предлагают своих, мирных не будят", &mafia_never_targets_allies);
    return test::finish();
}
