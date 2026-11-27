// Тесты оценок Beliefs на вручную собранных снимках: какие события как
// меняют оценки, переосмысление прошлого после раскрытия, «достоверно»
// против «подозреваю», и поведение ролей, которое на оценках построено.
#include "roles/beliefs.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

using namespace mafia;

namespace {

// Стол из n живых; события добавляются вызовами ниже.
struct World {
    explicit World(int n) {
        for (PlayerId i = 0; i < n; ++i) {
            PublicPlayer p;
            p.id = i;
            p.name = "Игрок " + std::to_string(i + 1);
            snap.players.push_back(p);
        }
        snap.day = 2;
    }
    void claim(PlayerId who, PlayerId about, bool mafia, int day = 1) {
        SpeechEvent s;
        s.speaker = who;
        s.text = "...";
        Claim c;
        c.about = about;
        c.mafia = mafia;
        s.claim = c;
        snap.history.push_back(HistoryEntry{day, Phase::Discussion, s});
    }
    void vote(PlayerId who, PlayerId target, int day = 1, int round = 1) {
        VoteEvent v;
        v.voter = who;
        v.target = target;
        v.round = round;
        snap.history.push_back(HistoryEntry{day, Phase::Voting, v});
    }
    void dies(PlayerId who, bool is_mafia, DeathCause cause, int day = 2) {
        auto& p = snap.players[static_cast<std::size_t>(who)];
        p.alive = false;
        Disclosure d;
        d.is_mafia = is_mafia;
        p.disclosed = d;
        DeathEvent e;
        e.victim = who;
        e.cause = cause;
        e.disclosed = d;
        snap.history.push_back(HistoryEntry{day, Phase::Morning, e});
    }
    Beliefs view(PlayerId self, RoleKind role, std::vector<KnownIdentity> known = {}) const {
        return Beliefs::evaluate(snap, self, role, known);
    }
    PublicSnapshot snap;
};

KnownIdentity fact(PlayerId id, bool mafia) {
    KnownIdentity k;
    k.id = id;
    k.is_mafia = mafia;
    return k;
}

}  // namespace

void accusations_and_votes() {
    World w(6);
    w.claim(0, 3, true);   // Игрок 1: «Игрок 4 — мафия»
    w.vote(1, 3);          // Игрок 2 голосует против Игрока 4
    w.vote(2, 3);
    w.claim(4, 5, false);  // Игрок 5: «Игрок 6 — мирный»
    const Beliefs b = w.view(-1, RoleKind::Civilian);
    CHECK(b.suspicion(3) > b.suspicion(0));
    CHECK(b.suspicion(3) > 2.0);   // два голоса (по 1.0) и обвинение (0.6)
    CHECK(b.suspicion(5) < 0);     // защита снижает подозрение
    CHECK(!b.certain_mafia(3));    // но это лишь оценка, не знание
}

void reveal_rewrites_history() {
    World w(6);
    w.claim(0, 3, true);   // Игрок 1 обвинял Игрока 4
    w.claim(1, 3, false);  // Игрок 2 его защищал
    w.vote(3, 4);          // Игрок 4 голосовал против Игрока 5
    w.vote(2, 4);          // Игрок 3 — вместе с ним
    const Beliefs before = w.view(-1, RoleKind::Civilian);

    w.dies(3, /*is_mafia=*/true, DeathCause::Executed);  // Игрок 4 оказался мафией
    const Beliefs after = w.view(-1, RoleKind::Civilian);

    CHECK(after.certain_mafia(3) == true);
    CHECK(after.trust(0) > before.trust(0));            // обвинявший — попал
    CHECK(after.suspicion(1) > before.suspicion(1) + 1.5);  // защищавший мафию — под подозрением
    CHECK(after.suspicion(4) < before.suspicion(4));    // против кого голосовала мафия — скорее честен
    CHECK(after.suspicion(2) > before.suspicion(2));    // голосовал заодно с мафиози
}

void certain_is_not_suspicion() {
    World w(6);
    // Все дружно обвиняют Игрока 3 и голосуют против него...
    w.claim(0, 2, true);
    w.vote(1, 2);
    w.vote(4, 2);
    // ...но комиссар (Игрок 6) проверил: Игрок 3 не мафия.
    const Beliefs com = w.view(5, RoleKind::Commissar, {fact(2, false)});
    const Beliefs town = w.view(-1, RoleKind::Civilian);
    CHECK(town.suspicion(2) > 2.0);                // город подозревает
    CHECK(com.certain_mafia(2) == false);          // комиссар знает: не мафия
    // ...и разговоры этого не меняют: подозрения стола к нему не применяются.
    // Но «не мафия» — ещё не «безопасен»: это может быть маньяк. Поэтому не −100,
    // а небольшой сдвиг к невиновности.
    CHECK(com.suspicion(2) < 0 && com.suspicion(2) > -Beliefs::kCertain);
    CHECK(com.suspicion(2) < town.suspicion(2));
    CHECK(com.suspicion(1) > town.suspicion(1));   // голосовавший против честного — подозрителен
    // Сам себя игрок знает всегда.
    CHECK(com.certain_mafia(5) == false);
    CHECK(w.view(3, RoleKind::Mafioso).certain_mafia(3) == true);
}

void false_accusation_of_me() {
    World w(6);
    w.claim(0, 2, true);  // Игрок 1: «Игрок 3 — мафия»
    // Игрок 3 — мирный и знает это: обвинитель для него подозрителен.
    const Beliefs me = w.view(2, RoleKind::Civilian);
    const Beliefs town = w.view(-1, RoleKind::Civilian);
    CHECK(me.suspicion(0) > town.suspicion(0));
}

void night_victim_points_at_enemies() {
    World w(6);
    w.claim(1, 4, true);  // Игрок 2 обвинял Игрока 5
    w.vote(1, 4);
    const double before = w.view(-1, RoleKind::Civilian).suspicion(4);
    w.dies(1, /*is_mafia=*/false, DeathCause::Night);  // Игрока 2 убили ночью
    CHECK(w.view(-1, RoleKind::Civilian).suspicion(4) > before);  // «кому он мешал?»
}

void maniac_hunt() {
    // Мафиози (Игрок 1) голосовал против Игрока 4, потом его казнили.
    World w(6);
    w.snap.mafia_total = 2;
    w.vote(0, 3);
    w.dies(0, /*is_mafia=*/true, DeathCause::Executed);
    const double one_mafia_left = w.view(-1, RoleKind::Civilian).suspicion(3);
    CHECK(one_mafia_left < 0);  // мафия против него — «скорее не мафия»

    // Когда раскрыта вся мафия, а игра идёт, — жив маньяк: «не мафия»
    // больше ничего не доказывает.
    w.snap.mafia_total = 1;
    const double hunt = w.view(-1, RoleKind::Civilian).suspicion(3);
    CHECK(hunt > one_mafia_left);
}

void mafia_threat() {
    World w(6);
    w.claim(0, 3, true);  // Игрок 1 давит на Игрока 4 (союзника мафии)
    w.vote(0, 3);
    w.claim(1, 2, true);  // Игрок 2 обвиняет мирного
    const Beliefs maf = w.view(4, RoleKind::Mafioso, {fact(3, true)});
    CHECK(maf.threat(0) > maf.threat(1));  // для мафии опасен тот, кто бьёт по своим
}

void mafioso_never_turns_on_allies() {
    // Город подозревает Игрока 2 (союзника) больше всех — мафиози всё равно
    // не голосует против него и не обвиняет его.
    World w(6);
    w.vote(0, 1);
    w.vote(2, 1);
    w.vote(5, 1);
    w.claim(0, 1, true);
    auto snap = make_shared_ptr<PublicSnapshot>(w.snap);
    SharedPtr<const PublicSnapshot> world = std::move(snap);

    SharedPtr<Player> m = PlayerFactory::create(RoleKind::Mafioso, PlayerSetup(3, "Игрок 4", Rng(5)));
    for (int i = 0; i < 30; ++i) {
        Request vote;
        vote.id = static_cast<std::uint64_t>(i);
        vote.kind = RequestKind::Vote;
        vote.world = world;
        vote.you.self = 3;
        vote.you.role = RoleKind::Mafioso;
        vote.you.known = {fact(1, true)};
        vote.targets = {0, 1, 2, 4, 5};
        CHECK(m->handle(vote).target != 1);

        Request speech = vote;
        speech.kind = RequestKind::Speech;
        speech.targets.clear();
        const Response s = m->handle(speech);
        CHECK(!s.claim || !s.claim->mafia || s.claim->about != 1);
    }
}

void civilian_votes_top_suspect() {
    World w(6);
    w.vote(0, 4);
    w.vote(1, 4);
    w.claim(2, 4, true);
    w.claim(3, 0, true);
    auto snap = make_shared_ptr<PublicSnapshot>(w.snap);
    SharedPtr<const PublicSnapshot> world = std::move(snap);
    SharedPtr<Player> c = PlayerFactory::create(RoleKind::Civilian, PlayerSetup(5, "Игрок 6", Rng(1)));
    Request vote;
    vote.id = 1;
    vote.kind = RequestKind::Vote;
    vote.world = world;
    vote.you.self = 5;
    vote.targets = {0, 1, 2, 3, 4};
    CHECK(c->handle(vote).target == 4);
    // И в речи обвиняет того же.
    Request speech = vote;
    speech.kind = RequestKind::Speech;
    const Response s = c->handle(speech);
    CHECK(s.claim && s.claim->mafia && s.claim->about == 4);
}

void doctor_does_not_hoard_self() {
    // Пустая история: у доктора нет причин выбирать себя чаще других
    // (регрессия: раньше «я точно не мафия» = −100 давало себе +50).
    World w(6);
    auto snap = make_shared_ptr<PublicSnapshot>(w.snap);
    SharedPtr<const PublicSnapshot> world = std::move(snap);
    int self = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        SharedPtr<Player> d = PlayerFactory::create(RoleKind::Doctor, PlayerSetup(0, "Игрок 1", Rng(seed)));
        Request night;
        night.id = 1;
        night.kind = RequestKind::NightMove;
        night.world = world;
        night.you.self = 0;
        night.you.role = RoleKind::Doctor;
        night.targets = {0, 1, 2, 3, 4, 5};
        night.actions = {NightActionKind::Heal};
        self += d->handle(night).night.target == 0 ? 1 : 0;
    }
    CHECK(self < 25);  // ~1/6 от 60, а не 60 из 60
}

int main() {
    std::cout << "Оценки игроков\n";
    test::run("обвинения и голоса поднимают подозрение", &accusations_and_votes);
    test::run("раскрытие переписывает прошлое", &reveal_rewrites_history);
    test::run("«знаю» и «подозреваю» — разные поля", &certain_is_not_suspicion);
    test::run("ложное обвинение меня — улика против обвинителя", &false_accusation_of_me);
    test::run("убитый ночью указывает на тех, кому мешал", &night_victim_points_at_enemies);
    test::run("вся мафия раскрыта — охота на маньяка", &maniac_hunt);
    test::run("опасность для мафии", &mafia_threat);
    test::run("мафиози не голосует и не обвиняет своих", &mafioso_never_turns_on_allies);
    test::run("мирный голосует против самого подозрительного", &civilian_votes_top_suspect);
    test::run("доктор не держит лечение для себя", &doctor_does_not_hoard_self);
    return test::finish();
}
