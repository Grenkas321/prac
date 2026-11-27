// Тесты дополнительных ролей.
//   1. Ночь по ролям — чистая функция resolve_full_night на вручную собранном
//      столе: каждая способность, иммунитет и порядок этапов.
//   2. Дневные способности и превращения — настоящий ведущий на нитях, игроки
//      со сценарием (их ночные ходы, голоса и выборы заданы заранее).
//   3. Партии со всеми 35 ролями сразу — на обоих движках итог совпадает.
#include "engine/coro_engine.hpp"
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "game/night_rules.hpp"
#include "roles/catalog.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

using namespace mafia;

namespace {

// ---------------------------------------------------------------------------
// 1. Стол для чистых проверок ночи
// ---------------------------------------------------------------------------

GameState table(std::initializer_list<RoleKind> roles, int night = 2) {
    GameState st;
    PlayerId i = 0;
    for (RoleKind r : roles) {
        PlayerState p;
        p.id = i;
        p.name = "И" + std::to_string(i);
        p.role = r;
        st.players.push_back(p);
        ++i;
    }
    st.day = night;
    return st;
}

NightOrder order(const GameState& st, PlayerId actor, NightActionKind kind, PlayerId target) {
    NightOrder o;
    o.actor = actor;
    o.role = st.players[static_cast<std::size_t>(actor)].role;
    o.action.kind = kind;
    o.action.target = target;
    return o;
}

NightOrder council(const GameState& st, PlayerId boss, PlayerId target) {
    NightOrder o = order(st, boss, NightActionKind::Kill, target);
    o.council = true;
    return o;
}

bool dies(const NightReport& r, PlayerId p) {
    return std::ranges::find(r.deaths, p) != r.deaths.end();
}

std::optional<KnownIdentity> fact_for(const NightReport& r, PlayerId to, PlayerId about) {
    for (const auto& [who, k] : r.facts) {
        if (who == to && k.id == about) {
            return k;
        }
    }
    return std::nullopt;
}

bool note_for(const NightReport& r, PlayerId to, const std::string& part) {
    return std::ranges::any_of(r.notes, [&](const auto& n) { return n.first == to && n.second.find(part) != std::string::npos; });
}

using R = RoleKind;
using A = NightActionKind;

}  // namespace

void guards() {
    // Телохранитель: спасает и убивает нападавшего.
    GameState st = table({R::Maniac, R::Civilian, R::Bodyguard});
    NightReport r = resolve_full_night(st, {order(st, 0, A::Kill, 1), order(st, 2, A::Guard, 1)});
    CHECK(!dies(r, 1) && dies(r, 0) && !dies(r, 2));
    // ...но от выстрела комиссара — просто спасает.
    st = table({R::Commissar, R::Civilian, R::Bodyguard});
    r = resolve_full_night(st, {order(st, 0, A::Shoot, 1), order(st, 2, A::Guard, 1)});
    CHECK(!dies(r, 1) && !dies(r, 0));
    // Самурай: гибнет вместо цели и убивает нападавшего (исполнителя решения мафии).
    st = table({R::Mafioso, R::Civilian, R::Samurai});
    r = resolve_full_night(st, {council(st, 0, 1), order(st, 2, A::Guard, 1)});
    CHECK(!dies(r, 1) && dies(r, 2) && dies(r, 0));
    // Реанимирующий комиссар: проверенного этой ночью убить нельзя.
    st = table({R::Maniac, R::Civilian, R::ReviveCommissar});
    r = resolve_full_night(st, {order(st, 0, A::Kill, 1), order(st, 2, A::Check, 1)});
    CHECK(!dies(r, 1) && fact_for(r, 2, 1));
}

void drunk() {
    // Хозяин пропускает ход: доктор не лечит.
    GameState st = table({R::Maniac, R::Civilian, R::Doctor, R::Drunk});
    NightReport r = resolve_full_night(st, {order(st, 0, A::Kill, 1), order(st, 2, A::Heal, 1), order(st, 3, A::Visit, 2)});
    CHECK(dies(r, 1));
    // Нападение на хозяина задевает и гостя.
    st = table({R::Maniac, R::Civilian, R::Drunk});
    r = resolve_full_night(st, {order(st, 0, A::Kill, 1), order(st, 2, A::Visit, 1)});
    CHECK(dies(r, 1) && dies(r, 2));
    // Самого пьяницу дома не застать.
    st = table({R::Maniac, R::Civilian, R::Drunk});
    r = resolve_full_night(st, {order(st, 0, A::Kill, 2), order(st, 2, A::Visit, 1)});
    CHECK(!dies(r, 2) && !dies(r, 1));
    // Пьяница у Босса — решение мафии сорвано.
    st = table({R::Mafioso, R::Civilian, R::Drunk});
    r = resolve_full_night(st, {council(st, 0, 1), order(st, 2, A::Visit, 0)});
    CHECK(!dies(r, 1));
}

void thief_slander_ninja_steal() {
    // Вор блокирует проверку комиссара.
    GameState st = table({R::Thief, R::Commissar, R::Mafioso});
    NightReport r = resolve_full_night(st, {order(st, 0, A::Block, 1), order(st, 1, A::Check, 2)});
    CHECK(!fact_for(r, 1, 2));
    // Пьяница у вора — вор не блокирует.
    st = table({R::Thief, R::Commissar, R::Mafioso, R::Drunk});
    r = resolve_full_night(st, {order(st, 0, A::Block, 1), order(st, 1, A::Check, 2), order(st, 3, A::Visit, 0)});
    CHECK(fact_for(r, 1, 2) && fact_for(r, 1, 2)->is_mafia);
    // Ниндзя при проверке — мирный; оговорённый баламутом мирный — мафия.
    st = table({R::Ninja, R::Commissar, R::Civilian, R::Slanderer});
    r = resolve_full_night(st, {order(st, 1, A::Check, 0)});
    CHECK(fact_for(r, 1, 0) && !fact_for(r, 1, 0)->is_mafia);
    r = resolve_full_night(st, {order(st, 3, A::Slander, 2), order(st, 1, A::Check, 2)});
    CHECK(fact_for(r, 1, 2) && fact_for(r, 1, 2)->is_mafia);
    // Жулик крадёт проверку: сведения — жулику, комиссару — ничего.
    st = table({R::Swindler, R::Commissar, R::Mafioso});
    r = resolve_full_night(st, {order(st, 0, A::Steal, 1), order(st, 1, A::Check, 2)});
    CHECK(fact_for(r, 0, 2) && !fact_for(r, 1, 2));
}

void poison() {
    // Ночь отравления — жертва жива; следующей ночью умирает.
    GameState st = table({R::Poisoner, R::Civilian, R::Doctor});
    NightReport r = resolve_full_night(st, {order(st, 0, A::Poison, 1)});
    CHECK(!dies(r, 1) && r.poisonings.size() == 1 && !r.poisonings[0].neutralized);
    st.extra.poisonings = r.poisonings;
    ++st.day;
    CHECK(dies(resolve_full_night(st, {}), 1));
    // Лечение на следующую ночь уже не помогает...
    CHECK(dies(resolve_full_night(st, {order(st, 2, A::Heal, 1)}), 1));
    // ...а противоядие помогает.
    CHECK(!dies(resolve_full_night(st, {order(st, 0, A::Antidote, 1)}), 1));
    // Доктор, пришедший в ночь отравления, обезвреживает яд сразу.
    st = table({R::Poisoner, R::Civilian, R::Doctor});
    r = resolve_full_night(st, {order(st, 0, A::Poison, 1), order(st, 2, A::Heal, 1)});
    CHECK(r.poisonings[0].neutralized);
    // Двое отравленных мирных — отравитель умирает сам.
    st = table({R::Poisoner, R::Civilian});
    st.extra.honest_kills[0] = 1;
    st.extra.poisonings.push_back(Poisoning{0, 1, 1, false});
    r = resolve_full_night(st, {});
    CHECK(dies(r, 1) && dies(r, 0));
}

void kidnap_kamikaze_immunity() {
    // Похититель угадал роль — лечение не спасает.
    GameState st = table({R::Kidnapper, R::Commissar, R::Doctor});
    NightOrder k = order(st, 0, A::Kidnap, 1);
    k.action.guess = R::Commissar;
    NightReport r = resolve_full_night(st, {k, order(st, 2, A::Heal, 1)});
    CHECK(dies(r, 1) && !dies(r, 0));
    // Ошибся — гибнет сам.
    k.action.guess = R::Doctor;
    r = resolve_full_night(st, {k});
    CHECK(!dies(r, 1) && dies(r, 0));
    // Камикадзе нашёл комиссара — гибнут оба; не нашёл — узнаёт «не комиссар».
    st = table({R::Kamikaze, R::Commissar, R::Civilian});
    r = resolve_full_night(st, {order(st, 0, A::Seek, 1)});
    CHECK(dies(r, 0) && dies(r, 1));
    r = resolve_full_night(st, {order(st, 0, A::Seek, 2)});
    CHECK(!dies(r, 0) && note_for(r, 0, "не комиссар"));
    // Иммунитеты: блейд ночью, бык от маньяка, подкидыш от мафии, заключённый.
    st = table({R::Maniac, R::Blade, R::Bull, R::Mafioso, R::Foundling, R::Civilian});
    CHECK(!dies(resolve_full_night(st, {order(st, 0, A::Kill, 1)}), 1));
    CHECK(!dies(resolve_full_night(st, {order(st, 0, A::Kill, 2)}), 2));
    CHECK(!dies(resolve_full_night(st, {council(st, 3, 4)}), 4));
    CHECK(dies(resolve_full_night(st, {order(st, 0, A::Kill, 4)}), 4));  // маньяку подкидыш по зубам
    st.extra.jailed.insert(5);
    CHECK(!dies(resolve_full_night(st, {order(st, 0, A::Kill, 5)}), 5));
    // Блейд, убивший троих мирных, гибнет сам.
    st = table({R::Blade, R::Civilian});
    st.extra.honest_kills[0] = 2;
    NightReport b = resolve_full_night(st, {order(st, 0, A::Kill, 1)});
    CHECK(dies(b, 1) && dies(b, 0));
}

void information_roles() {
    // Священник узнаёт статус, исповедуемый узнаёт священника.
    GameState st = table({R::Priest, R::Mafioso, R::Spy, R::Commissar, R::Advocate, R::Maniac, R::Jailer, R::Donor,
                          R::Civilian, R::Journalist, R::Ears, R::Witness, R::Fan, R::Hacker, R::Paranoid});
    NightReport r = resolve_full_night(st, {order(st, 0, A::Check, 1)});
    CHECK(fact_for(r, 0, 1)->is_mafia && fact_for(r, 1, 0)->role == R::Priest);
    // Шпион: результат — комиссару, не шпиону.
    r = resolve_full_night(st, {order(st, 2, A::Check, 1)});
    CHECK(fact_for(r, 3, 1) && !fact_for(r, 2, 1));
    // Адвокат узнаёт полный статус, в том числе маньяка.
    r = resolve_full_night(st, {order(st, 4, A::Check, 5)});
    CHECK(fact_for(r, 4, 5)->role == R::Maniac);
    // Тюремщик сажает нечестного (мафию или маньяка), честного — нет.
    r = resolve_full_night(st, {order(st, 6, A::Check, 5)});
    CHECK(r.jailed == std::vector<PlayerId>({5}));
    CHECK(resolve_full_night(st, {order(st, 6, A::Check, 8)}).jailed.empty());
    // Донор: честный проверенный получает проверку на следующую ночь.
    CHECK(resolve_full_night(st, {order(st, 7, A::Check, 8)}).granted_check == std::vector<PlayerId>({8}));
    CHECK(resolve_full_night(st, {order(st, 7, A::Check, 1)}).granted_check.empty());
    // Журналист сравнивает двоих.
    NightOrder j = order(st, 9, A::Compare, 1);
    j.action.second = 4;
    CHECK(note_for(resolve_full_night(st, {j}), 9, "одинаковый"));
    j.action.second = 8;
    CHECK(note_for(resolve_full_night(st, {j}), 9, "разный"));
    // Ушастик слышит действия без имён; свидетель видит нападавшего.
    r = resolve_full_night(st, {order(st, 10, A::Listen, 8), order(st, 11, A::Watch, 8), order(st, 5, A::Kill, 8),
                                order(st, 0, A::Check, 8)});
    CHECK(note_for(r, 10, "убийство") && note_for(r, 10, "проверка"));
    CHECK(fact_for(r, 11, 5) && fact_for(r, 11, 5)->role == R::Maniac);
    // Поклонница находит комиссара.
    r = resolve_full_night(st, {order(st, 12, A::Admire, 3)});
    CHECK(fact_for(r, 12, 3)->role == R::Commissar && r.fan_done == std::vector<PlayerId>({12}));
    // Хакер: результат взлома — всем.
    r = resolve_full_night(st, {order(st, 13, A::Hack, 1)});
    CHECK(r.reveals.size() == 1 && r.reveals[0].second.role == R::Mafioso);
    // Параноик узнаёт о проверке комиссаром.
    CHECK(note_for(resolve_full_night(st, {order(st, 3, A::Check, 14)}), 14, "проверял комиссар"));
}

void cattani_and_outputs() {
    // Каттани проверил убитого этой ночью — узнаёт убийцу.
    GameState st = table({R::Cattani, R::Civilian, R::Maniac, R::Prankster, R::Boss, R::Reanimator});
    st.players[1].alive = true;
    NightReport r = resolve_full_night(st, {order(st, 2, A::Kill, 1), order(st, 0, A::Check, 1)});
    CHECK(dies(r, 1) && fact_for(r, 0, 2) && fact_for(r, 0, 2)->role == R::Maniac);
    // Громила, босс, реаниматор — их результат идёт в отчёт.
    st.players[1].alive = false;
    r = resolve_full_night(st, {order(st, 3, A::Prank, 0), order(st, 4, A::Freeze, 0), order(st, 5, A::Revive, 1)});
    CHECK(r.prank == 0 && r.freeze == 0 && r.revived == std::vector<PlayerId>({1}));
}

// ---------------------------------------------------------------------------
// 2. Ведущий со сценарием: дневные способности и превращения
// ---------------------------------------------------------------------------

namespace {

struct Script {
    std::map<std::pair<int, PlayerId>, NightAction> night;  // (ночь, игрок) -> ход
    std::map<PlayerId, PlayerId> vote_for;                  // голосующий -> против кого
    std::optional<PlayerId> scapegoat;                      // против кого все остальные
    std::map<PlayerId, std::size_t> choice;                 // ответ на Choice
};
Script g_script;  // пишет главная нить до exchange, читают нити игроков внутри него

template <class Base>
class Scripted : public Base {
public:
    using Base::Base;

    Response act(const Request& request) {
        auto it = g_script.night.find({request.world->day, this->id()});
        if (it != g_script.night.end() && std::ranges::find(request.actions, it->second.kind) != request.actions.end()) {
            Response r;
            r.night = it->second;
            return r;
        }
        if constexpr (requires(Base& b) { b.act(request); }) {
            return Base::act(request);
        } else {
            return Response{};
        }
    }

protected:
    int target_score(const Request& request, Player::Purpose purpose, PlayerId c) const override {
        if (purpose == Player::Purpose::Vote) {
            if (auto it = g_script.vote_for.find(this->id()); it != g_script.vote_for.end()) {
                return it->second == c ? 1'000'000 : 0;
            }
            if (g_script.scapegoat == c) {
                return 1'000'000;
            }
        }
        return Base::target_score(request, purpose, c);
    }
    std::size_t choose_option(const Request& request) override {
        if (auto it = g_script.choice.find(this->id()); it != g_script.choice.end()) {
            return it->second;
        }
        return Base::choose_option(request);
    }
};

using Maker = SharedPtr<Player> (*)(PlayerSetup);
const auto kScriptedMakers = []<std::size_t... I>(std::index_sequence<I...>) {
    return std::array<Maker, kRoleCount>{&PlayerFactory::create_as<Scripted<RoleClassOf_t<kAllRoles[I]>>>...};
}(std::make_index_sequence<kRoleCount>{});

struct Game {
    Game(int players, std::vector<RoleKind> extra, std::uint64_t seed = 3) {
        g_script = Script{};
        HostConfig c;
        c.players = players;
        c.seed = seed;
        c.extra_roles = std::move(extra);
        c.execute_on_first_day = true;
        c.make_player = [](RoleKind role, PlayerSetup setup) {
            return kScriptedMakers[static_cast<std::size_t>(role)](std::move(setup));
        };
        host = std::make_unique<Host>(c, make_shared_ptr<ThreadEngine>(), out);
        host->setup();
    }
    ~Game() { host->shutdown(); }

    PlayerId who(RoleKind r) const {
        for (const PlayerState& p : host->state().players) {
            if (p.role == r) {
                return p.id;
            }
        }
        return -1;
    }
    std::vector<PlayerId> all(RoleKind r) const {
        std::vector<PlayerId> out;
        for (const PlayerState& p : host->state().players) {
            if (p.role == r) {
                out.push_back(p.id);
            }
        }
        return out;
    }
    const PlayerState& p(PlayerId id) const { return host->state().players[static_cast<std::size_t>(id)]; }
    void night(int n, PlayerId actor, NightActionKind kind, PlayerId target) {
        NightAction a;
        a.kind = kind;
        a.target = target;
        g_script.night[{n, actor}] = a;
    }
    // Все голосуют против target (сам target — против fallback).
    void everyone_votes(PlayerId target, PlayerId fallback) {
        g_script.vote_for.clear();
        for (const PlayerState& q : host->state().players) {
            g_script.vote_for[q.id] = q.id == target ? fallback : target;
        }
    }
    bool said(const std::string& part) const { return out.str().find(part) != std::string::npos; }

    std::ostringstream out;
    std::unique_ptr<Host> host;
};

}  // namespace

void elder_cannot_be_executed() {
    Game g(10, {R::Elder});
    const PlayerId elder = g.who(R::Elder);
    g.everyone_votes(elder, g.who(R::Civilian));
    g.host->run_day();
    CHECK(g.p(elder).alive);
    CHECK(g.said("старейшина, его нельзя казнить"));
    CHECK(g.p(elder).disclosed && g.p(elder).disclosed->role == R::Elder);  // и он раскрыт
}

void executioner_breaks_tie() {
    Game g(10, {R::Executioner});
    const std::vector<PlayerId> civ = g.all(R::Civilian);
    // Гарантированная ничья 5:5: восемь остальных делятся 4 на 4, а лидеры
    // голосуют друг против друга. Палач казнит civ[0] (вариант 0).
    int others = 0;
    for (const PlayerState& q : g.host->state().players) {
        if (q.id == civ[0]) {
            g_script.vote_for[q.id] = civ[1];
        } else if (q.id == civ[1]) {
            g_script.vote_for[q.id] = civ[0];
        } else {
            g_script.vote_for[q.id] = (others++ % 2 == 0) ? civ[0] : civ[1];
        }
    }
    g_script.choice[g.who(R::Executioner)] = 0;
    g.host->run_day();
    CHECK(g.said("Итоги тура 1: " + g.p(std::min(civ[0], civ[1])).name + " — 5;"));
    CHECK(g.said("палач выносит приговор: " + g.p(civ[0]).name));
    CHECK(!g.p(civ[0]).alive && g.p(civ[1]).alive);
}

void jailer_and_prankster_and_boss() {
    Game g(12, {R::Jailer, R::Prankster, R::Boss});
    const PlayerId jailer = g.who(R::Jailer);
    const PlayerId prankster = g.who(R::Prankster);
    const PlayerId boss = g.who(R::Boss);
    const PlayerId maf = g.who(R::Mafioso);
    const std::vector<PlayerId> civ = g.all(R::Civilian);
    g.everyone_votes(civ[0], civ[1]);
    g.host->run_day();  // день 1: казнят civ[0]
    g.night(1, jailer, A::Check, maf);         // тюремщик сажает мафиози
    g.night(1, prankster, A::Prank, civ[1]);   // завтра civ[1] молчит
    g.night(1, boss, A::Freeze, civ[2]);       // голос civ[2] завтра не считается
    g.host->run_night();
    g.everyone_votes(civ[3], civ[1]);
    g.host->run_day();
    CHECK(g.said(g.p(maf).name + " в тюрьме"));
    CHECK(g.said(g.p(civ[1]).name + " голосует смайликом"));
    CHECK(g.said("жертва шутки громилы"));
    // Заключённый не голосует: среди голосов дня 2 его нет.
    bool jailed_voted = false;
    for (const HistoryEntry& h : g.host->state().history) {
        if (const auto* v = std::get_if<VoteEvent>(&h.event); v && h.day == 2 && v->voter == maf) {
            jailed_voted = true;
        }
    }
    CHECK(!jailed_voted);
}

void transformations() {
    // Комиссар выбыл — сержант становится комиссаром.
    Game g(12, {R::Sergeant, R::Foundling, R::Werewolf});
    const PlayerId com = g.who(R::Commissar);
    const PlayerId sgt = g.who(R::Sergeant);
    CHECK(sgt >= 0 && com >= 0);
    g.everyone_votes(com, g.who(R::Civilian));
    g.host->run_day();
    CHECK(!g.p(com).alive && g.p(sgt).role == R::Commissar);
    // Сержант с самого начала знал комиссара.
    const Briefing b = g.host->briefing_for(sgt);
    CHECK(std::ranges::any_of(b.known, [&](const KnownIdentity& k) { return k.id == com && k.role == R::Commissar; }));
}

void foundling_and_werewolf() {
    Game g(9, {R::Foundling, R::Werewolf});  // 3 мафии: оборотень + 2 обычных
    const PlayerId foundling = g.who(R::Foundling);
    const PlayerId werewolf = g.who(R::Werewolf);
    const std::vector<PlayerId> maf = g.all(R::Mafioso);
    CHECK(maf.size() == 2);
    // Казним Босса (первого живого мафиози) — подкидыш становится мафиози.
    // Босс — первый живой мафиози по рассадке; спящий оборотень Боссом не бывает.
    const PlayerId boss = std::min(maf[0], maf[1]);
    g.everyone_votes(boss, foundling);
    g.host->run_day();
    CHECK(!g.p(boss).alive);
    CHECK(g.p(foundling).role == R::Mafioso);
    // Подкидыш теперь знает своих — в том числе оборотня.
    const Briefing b = g.host->briefing_for(foundling);
    CHECK(std::ranges::any_of(b.known, [&](const KnownIdentity& k) { return k.id == werewolf && k.is_mafia; }));
}

void blade_and_cattani_revenge() {
    Game g(12, {R::Blade, R::Cattani});
    const PlayerId blade = g.who(R::Blade);
    g.everyone_votes(blade, g.who(R::Civilian));
    g.host->run_day();
    CHECK(!g.p(blade).alive);
    CHECK(g.said("был блейдом и забирает с собой"));
    int revenge = 0;
    for (const HistoryEntry& h : g.host->state().history) {
        if (const auto* d = std::get_if<DeathEvent>(&h.event); d && d->cause == DeathCause::Revenge) {
            ++revenge;
        }
    }
    CHECK(revenge >= 1);

    Game c(12, {R::Cattani});
    const PlayerId cattani = c.who(R::Cattani);
    c.everyone_votes(cattani, c.who(R::Civilian));
    c.host->run_day();
    CHECK(!c.p(cattani).alive && c.said("комиссар Каттани — успевает выстрелить"));
}

void necromancer_votes_and_revives() {
    Game g(12, {R::Necromancer});
    const PlayerId necro = g.who(R::Necromancer);
    const std::vector<PlayerId> civ = g.all(R::Civilian);
    g.everyone_votes(civ[0], civ[1]);
    g_script.choice[necro] = 99;  // в первый день — «не сейчас» (последний вариант подставит ведущий)
    g.host->run_day();
    CHECK(!g.p(civ[0]).alive);
    g.host->run_night();
    g.everyone_votes(civ[1], civ[2]);
    g_script.choice.erase(necro);
    g_script.choice[necro] = 99;
    g.host->run_day();
    // Выбывший мирный голосовал половиной голоса.
    CHECK(g.said("полголоса из-за некроманта"));
}

void config_deck() {
    // Мафиозные роли замещают мафиози, остальные — мирных; один мафиози остаётся обычным.
    Game g(12, {R::Killer, R::Thief, R::Bodyguard, R::Hacker});
    std::map<RoleKind, int> count;
    for (const PlayerState& p : g.host->state().players) {
        ++count[p.role];
    }
    CHECK(count[R::Killer] == 1 && count[R::Thief] == 1 && count[R::Mafioso] == 2);  // 4 мафии = 2 особых + 2
    CHECK(count[R::Bodyguard] == 1 && count[R::Hacker] == 1 && count[R::Civilian] == 3);  // 12-4-3 = 5 мест
    CHECK(g.said("Дополнительные роли: Киллер, Вор, Телохранитель, Хакер"));

    auto setup_error = [](int n, std::vector<RoleKind> extra) {
        HostConfig c;
        c.players = n;
        c.extra_roles = std::move(extra);
        std::ostringstream out;
        Host h(c, make_shared_ptr<CoroEngine>(), out);
        try {
            h.setup();
        } catch (const std::invalid_argument& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    // 8 игроков -> 2 мафии; две мафиозные роли не поместятся (один должен остаться обычным).
    CHECK(setup_error(8, {R::Killer, R::Thief}).find("хотя бы один мафиози") != std::string::npos);
    CHECK(setup_error(6, {R::Spy, R::Fan}).find("мест мирных") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. Все 35 ролей сразу — на обоих движках
// ---------------------------------------------------------------------------

namespace {

std::vector<RoleKind> all_extra_roles() {
    std::vector<RoleKind> out;
    for (RoleKind r : kAllRoles) {
        if (std::ranges::find(kBaseRoles, r) == kBaseRoles.end()) {
            out.push_back(r);
        }
    }
    return out;
}

std::string play(SharedPtr<Engine> engine, int players, std::vector<RoleKind> extra, std::uint64_t seed,
                 GameState* state = nullptr, std::set<RoleKind>* dealt = nullptr) {
    HostConfig c;
    c.players = players;
    c.seed = seed;
    c.extra_roles = std::move(extra);
    std::ostringstream out;
    Host host(c, engine, out);
    host.setup();
    if (dealt) {
        for (const PlayerState& p : host.state().players) {
            dealt->insert(p.role);  // раздача — до превращений по ходу партии
        }
    }
    host.play();
    host.shutdown();
    if (state) {
        *state = host.state();
    }
    std::string protocol;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with("Исполнение: ")) {
            protocol += line + "\n";
        }
    }
    return protocol;
}

}  // namespace

void all_roles_both_engines() {
    const std::vector<RoleKind> extra = all_extra_roles();
    CHECK(extra.size() == 35);
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        GameState threads_state;
        GameState coro_state;
        std::set<RoleKind> seen;
        const std::string threads = play(make_shared_ptr<ThreadEngine>(), 40, extra, seed, &threads_state, &seen);
        const std::string coros = play(make_shared_ptr<CoroEngine>(), 40, extra, seed, &coro_state);
        CHECK(threads == coros);
        CHECK(threads_state.finished && coro_state.finished);
        CHECK(threads_state.night_log.size() == coro_state.night_log.size());
        CHECK(threads.find("RESULT winner=") != std::string::npos);
        // Все роли действительно были за столом.
        // Розданы все 35 доп. ролей (обычных мирных при N = 40 не остаётся: все 24
        // места мирных заняты доп. ролями).
        CHECK(std::ranges::all_of(extra, [&](RoleKind r) { return seen.contains(r); }));
        CHECK(!seen.contains(RoleKind::Civilian) && seen.contains(RoleKind::Mafioso));
    }
}

void each_role_alone_both_engines() {
    // Каждая доп. роль по отдельности на 12 игроках (k = 3): партия доходит до конца,
    // нити и корутины дают один и тот же протокол.
    int games = 0;
    for (RoleKind r : all_extra_roles()) {
        const std::string threads = play(make_shared_ptr<ThreadEngine>(), 12, {r}, 7);
        const std::string coros = play(make_shared_ptr<CoroEngine>(), 12, {r}, 7);
        CHECK(threads == coros);
        CHECK(threads.find("RESULT winner=") != std::string::npos);
        ++games;
    }
    CHECK(games == 35);
}

int main() {
    std::cout << "Дополнительные роли\n";
    test::run("ночь: телохранитель, самурай, реанимирующий комиссар", &guards);
    test::run("ночь: пьяница", &drunk);
    test::run("ночь: вор, баламут, ниндзя, жулик", &thief_slander_ninja_steal);
    test::run("ночь: отравитель", &poison);
    test::run("ночь: похититель, камикадзе, иммунитеты, блейд", &kidnap_kamikaze_immunity);
    test::run("ночь: роли сведений", &information_roles);
    test::run("ночь: Каттани, громила, босс, реаниматор", &cattani_and_outputs);
    test::run("день: старейшину нельзя казнить", &elder_cannot_be_executed);
    test::run("день: палач при ничьей", &executioner_breaks_tie);
    test::run("тюрьма, шутка громилы, заморозка босса", &jailer_and_prankster_and_boss);
    test::run("превращение: сержант -> комиссар", &transformations);
    test::run("превращение: подкидыш -> мафиози", &foundling_and_werewolf);
    test::run("месть блейда и Каттани", &blade_and_cattani_revenge);
    test::run("некромант: полголоса выбывших", &necromancer_votes_and_revives);
    test::run("раздача: доп. роли замещают мафиози и мирных", &config_deck);
    test::run("все 35 ролей сразу: нити = корутины (4 зерна)", &all_roles_both_engines);
    test::run("каждая роль отдельно: нити = корутины (35 партий)", &each_role_alone_both_engines);
    return test::finish();
}
