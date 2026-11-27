#include "game/host.hpp"

#include "game/night.hpp"
#include "game/voting.hpp"
#include "roles/catalog.hpp"
#include "roles/doctor.hpp"
#include "roles/player_factory.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mafia {

namespace {

constexpr int kMaxAttempts = 3;  // сколько раз переспрашивать при недопустимом ответе

// Не «quoted»: с <iomanip> по аргументу std::string поиск (ADL) находил
// std::quoted и печатал прямые кавычки вместо наших.
std::string in_guillemets(const std::string& text) {
    return "«" + (text.empty() ? std::string("(молчит)") : text) + "»";
}

// Открытое объявление «кем убит»: по роли нападавшего, в творительном падеже.
std::string killer_phrase(RoleKind killer) {
    switch (killer) {
        case RoleKind::Maniac:
            return "маньяком";
        case RoleKind::Poisoner:
            return "отравителем (яд)";
        case RoleKind::Commissar:
        case RoleKind::Cattani:
            return "выстрелом комиссара";
        case RoleKind::Blade:
            return "блейдом";
        default:
            break;
    }
    return team_of(killer) == Team::Mafia ? "мафией" : "игроком с ролью «" + std::string(role_name(killer)) + "»";
}

// Голоса считаются в полуголосах (выбывший мирный при некроманте — половина).
std::string votes_text(int halves) {
    return std::to_string(halves / 2) + (halves % 2 != 0 ? ",5" : "");
}

}  // namespace

Host::Host(HostConfig config, SharedPtr<Engine> engine, std::ostream& out)
    : config_(config),
      engine_(std::move(engine)),
      console_(out),
      log_(config_.log),
      tee_(config_.quiet ? nullptr : console_.rdbuf(), log_.get()),  // тихо — только в файл
      out_(&tee_),
      state_(make_shared_ptr<GameState>()),
      rng_(Rng::stream(config.seed, 0)) {}

Host::~Host() {
    if (started_) {
        try {
            engine_->stop();  // без Stop-запросов: из деструктора нельзя бросать
        } catch (...) {
        }
    }
}

int Host::mafia_count(int players, int divisor) {
    if (players <= 4) {
        throw std::invalid_argument("игроков должно быть больше 4, задано " + std::to_string(players));
    }
    if (divisor < 3) {
        throw std::invalid_argument("делитель k должен быть не меньше 3, задано " + std::to_string(divisor));
    }
    return std::max(1, players / divisor);  // целочисленное деление — это и есть floor
}

// ---------------------------------------------------------------------------
// Подготовка
// ---------------------------------------------------------------------------

void Host::setup() {
    if (started_) {
        throw std::logic_error("Host::setup: партия уже подготовлена");
    }
    const int n = config_.players;
    const int mafia = mafia_count(n, config_.mafia_divisor);

    // Дополнительные роли: мафиозные замещают обычных мафиози (число мафии
    // остаётся floor(N/k)), остальные — мирных жителей.
    std::vector<RoleKind> mafia_extra;
    std::vector<RoleKind> civil_extra;
    for (RoleKind r : config_.extra_roles) {
        (team_of(r) == Team::Mafia ? mafia_extra : civil_extra).push_back(r);
    }
    if (static_cast<int>(mafia_extra.size()) > mafia - 1) {
        throw std::invalid_argument("мафиози в партии — " + std::to_string(mafia) + ", мафиозных доп. ролей — " +
                                    std::to_string(mafia_extra.size()) +
                                    ": хотя бы один мафиози должен остаться обычным (больше игроков или меньше k)");
    }
    const int civil_slots = n - mafia - 3;
    if (static_cast<int>(civil_extra.size()) > civil_slots) {
        throw std::invalid_argument("мест мирных жителей — " + std::to_string(civil_slots) + ", доп. ролей для них — " +
                                    std::to_string(civil_extra.size()) + " (нужно больше игроков)");
    }

    // Колода: мафия (особые + обычные), обязательные роли, доп. роли мирных, мирные.
    std::vector<RoleKind> deck = mafia_extra;
    deck.resize(static_cast<std::size_t>(mafia), RoleKind::Mafioso);
    deck.push_back(RoleKind::Commissar);
    deck.push_back(RoleKind::Doctor);
    deck.push_back(RoleKind::Maniac);
    deck.insert(deck.end(), civil_extra.begin(), civil_extra.end());
    const int civilians = n - static_cast<int>(deck.size());
    deck.resize(static_cast<std::size_t>(n), RoleKind::Civilian);
    rng_.shuffle(deck);

    for (PlayerId i = 0; i < n; ++i) {
        PlayerState p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        p.role = deck[static_cast<std::size_t>(i)];
        state_->players.push_back(p);
        PlayerSetup setup(i, p.name, Rng::stream(config_.seed, static_cast<std::uint64_t>(i) + 1));
        SharedPtr<Player> player = config_.make_player ? config_.make_player(p.role, std::move(setup))
                                                       : PlayerFactory::create(p.role, std::move(setup));
        if (!player || player->id() != i || player->role() != p.role) {
            throw std::logic_error("Host::setup: создан игрок не с тем номером или ролью");
        }
        players_.push_back(player);
    }
    state_->day = 0;
    state_->phase = Phase::Setup;

    // ИИ-места: поток n + 1 (0 — ведущий, 1..n — игроки) — не задевает остальных.
    std::vector<PlayerId> ai_seats;
    if (config_.ai_players > 0 && config_.make_advisor) {
        if (config_.ai_players > n) {
            throw std::invalid_argument("ИИ-игроков " + std::to_string(config_.ai_players) + ", а всего игроков " +
                                        std::to_string(n));
        }
        std::vector<PlayerId> seats(static_cast<std::size_t>(n));
        for (PlayerId i = 0; i < n; ++i) {
            seats[static_cast<std::size_t>(i)] = i;
        }
        Rng seat_rng = Rng::stream(config_.seed, static_cast<std::uint64_t>(n) + 1);
        seat_rng.shuffle(seats);
        if (config_.human_seat) {
            std::erase(seats, *config_.human_seat);  // место пользователя ИИ не отдаётся
        }
        if (config_.ai_players > static_cast<int>(seats.size())) {
            throw std::invalid_argument("ИИ-игроков больше, чем мест для ботов");
        }
        seats.resize(static_cast<std::size_t>(config_.ai_players));
        std::ranges::sort(seats);
        for (std::size_t k = 0; k < seats.size(); ++k) {
            players_[static_cast<std::size_t>(seats[k])]->set_advisor(
                config_.make_advisor(seats[k], static_cast<int>(k)));
        }
        ai_seats = std::move(seats);
    }

    // Сержант и комиссар знают друг друга с самого начала.
    if (std::optional<PlayerId> com = find_role(RoleKind::Commissar)) {
        for (const PlayerState& s : state_->players) {
            if (s.role == RoleKind::Sergeant) {
                tell(s.id, KnownIdentity{*com, false, RoleKind::Commissar});
                tell(*com, KnownIdentity{s.id, false, RoleKind::Sergeant});
            }
        }
    }
    last_boss_ = boss();

    engine_->start(players_);
    started_ = true;

    if (log_) {
        log_->begin_round(1);  // подготовка — часть первого круга
    }
    out_ << "=== Мафия: " << n << " игроков ===\n"
         << "Исполнение: " << engine_->name() << "; зерно партии: " << config_.seed << "\n"
         << "Состав: мафиози — " << mafia << ", комиссар — 1, доктор — 1, маньяк — 1, мирные жители — "
         << civilians << ".\n";
    if (!config_.extra_roles.empty()) {
        out_ << "Дополнительные роли:";
        for (std::size_t i = 0; i < config_.extra_roles.size(); ++i) {
            out_ << (i == 0 ? " " : ", ") << role_name(config_.extra_roles[i]);
        }
        out_ << " (мафиозные — вместо обычных мафиози, остальные — вместо мирных).\n";
    }
    out_ << "За столом:";
    for (const PlayerState& p : state_->players) {
        out_ << (p.id == 0 ? " " : ", ") << p.name;
    }
    out_ << ".\n";
    if (!ai_seats.empty()) {
        // Кто ИИ — не тайна: это способ игры, а не роль.
        out_ << "ИИ-агенты:";
        for (std::size_t k = 0; k < ai_seats.size(); ++k) {
            out_ << (k == 0 ? " " : ", ") << state_->players[static_cast<std::size_t>(ai_seats[k])].name;
        }
        out_ << " (при сбое модели решают обычной эвристикой).\n";
    }
    out_ << "Объявления ведущего: "
         << (config_.open_announcements ? "открытые — роль выбывшего, кто убил, кого спасли"
                                        : "закрытые — о выбывшем только «мафия / не мафия»")
         << ".\n";
    if (config_.human_seat) {
        out_ << "Пользователь играет за: " << name(*config_.human_seat) << ".\n";  // место — не тайна
    }
    out_ << "Ведущий: роли розданы. Мафия знает своих. Город засыпает до утра первого дня.\n";
    if (config_.human_seat) {
        // Роль пользователя — тайна: только ему в консоль, мимо общего протокола.
        const PlayerState& me = state_->players[static_cast<std::size_t>(*config_.human_seat)];
        console_ << "\n*** Вы — " << me.name << ". Ваша роль: " << role_name(me.role) << " ("
                 << team_name(team_of(me.role)) << ").\n    " << role_description(me.role) << "\n";
        for (const KnownIdentity& k : briefing_for(me.id).known) {
            console_ << "    Вы знаете: " << name(k.id) << " — "
                     << (k.role ? std::string(role_name(*k.role)) : std::string(k.is_mafia ? "мафия" : "не мафия"))
                     << ".\n";
        }
        console_ << "***\n" << std::flush;
    }
    if (log_) {
        out_ << "Журнал партии: " << log_->dir().string()
             << (log_->level() == GameLog::Level::Full ? " (полный)" : " (краткий)") << "\n";
        for (const PlayerState& p : state_->players) {
            log_->secret(p.name + " — " + std::string(role_name(p.role)));  // раздача — тайна до конца
        }
    }
    record(AnnouncementEvent{"Роли розданы, игра начинается."});
}

Briefing Host::briefing_for(PlayerId id) const {
    const PlayerState& me = state_->players.at(static_cast<std::size_t>(id));
    Briefing b;
    b.self = id;
    b.role = me.role;
    if (team_of(me.role) == Team::Mafia) {
        // Союзников знает только ведущий: роли лежат в GameState, а игроку
        // GameState не передаётся. Пересчитывается при каждом запросе —
        // поэтому подкидыш и оборотень, став мафией, сразу узнают своих.
        for (const PlayerState& p : state_->players) {
            if (p.id != id && team_of(p.role) == Team::Mafia) {
                b.known.push_back(KnownIdentity{p.id, true, p.role});
            }
            if (p.role == RoleKind::Foundling) {
                b.known.push_back(KnownIdentity{p.id, false, RoleKind::Foundling});  // мафия знает подкидыша
            }
        }
    }
    // Накопленные личные сведения: проверки, свидетельства, сержант/комиссар.
    // Записаны ночью с учётом того, КАК цель выглядела (ниндзя, оговор).
    if (auto it = state_->extra.facts.find(id); it != state_->extra.facts.end()) {
        b.known.insert(b.known.end(), it->second.begin(), it->second.end());
    }
    if (auto it = state_->extra.notes.find(id); it != state_->extra.notes.end()) {
        b.notes = it->second;
    }
    return b;
}

// ---------------------------------------------------------------------------
// День
// ---------------------------------------------------------------------------

void Host::run_day() {
    if (!started_) {
        throw std::logic_error("Host::run_day: партия не подготовлена");
    }
    if (state_->finished) {
        return;
    }
    ++state_->day;
    if (log_ && state_->day > 1) {
        out_.flush();
        log_->begin_round(state_->day);  // новый круг: утро, день и следующая ночь
    }
    if (config_.quiet) {
        console_ << '.' << std::flush;  // прогресс: точка на каждый день
    }
    out_ << "\n=== День " << state_->day << " ===\n";
    morning();  // сначала смерти прошлой ночи: мёртвые не говорят и не голосуют
    // Проверка №1: после ночи. Если ночь решила исход — дня не будет.
    if (check_end()) {
        return;
    }
    // Стартовый говорящий — следующий живой после вчерашнего стартового.
    // В первый день — место 0 (или следующий живой после последнего места).
    const int n = static_cast<int>(state_->players.size());
    first_speaker_ = next_alive_after(state_->day == 1 ? n - 1 : first_speaker_);
    discussion();
    if (state_->day == 1 && !config_.execute_on_first_day) {
        out_ << "Ведущий: первый день — день знакомства. Голосования нет.\n";
        record(AnnouncementEvent{"Первый день — без голосования."});
        return;
    }
    necromancer_turn();  // раз за игру может раскрыться и воскресить выбывшего
    voting();
    // Проверка №2: после казни. Если казнь решила исход — ночи не будет.
    check_end();
}

bool Host::is_alive(PlayerId id) const {
    return state_->players[static_cast<std::size_t>(id)].alive;
}

// Места по кругу, начиная с from: from, from+1, ..., с переходом через конец стола.
// Ленивое представление: номера вычисляются по одному, когда их спрашивают.
auto Host::seats_from(PlayerId from) const {
    const int n = static_cast<int>(state_->players.size());
    return std::views::iota(0, n) | std::views::transform([from, n](int step) { return (from + step) % n; });
}

PlayerId Host::next_alive_after(PlayerId seat) const {
    // drop(1) — сам seat не в счёт; filter — живые. Цепочка ленивая: проверка
    // останавливается на первом живом, остальные места не трогаются.
    auto alive_after = seats_from(seat) | std::views::drop(1) |
                       std::views::filter([this](PlayerId s) { return is_alive(s); });
    auto first = std::ranges::begin(alive_after);
    return first != std::ranges::end(alive_after) ? *first : seat;  // seat — если живых больше нет
}

std::vector<PlayerId> Host::speaking_order() const {
    auto order = seats_from(first_speaker_) | std::views::filter([this](PlayerId s) { return is_alive(s); });
    return {std::ranges::begin(order), std::ranges::end(order)};  // один проход, без промежуточных векторов
}

std::vector<PlayerId> Host::alive_except(PlayerId self) const {
    auto ids = state_->players |
               std::views::filter([self](const PlayerState& p) { return p.alive && p.id != self; }) |
               std::views::transform(&PlayerState::id);
    return {std::ranges::begin(ids), std::ranges::end(ids)};
}

void Host::discussion() {
    state_->phase = Phase::Discussion;
    const std::vector<PlayerId> order = speaking_order();
    out_ << "Обсуждение. Первым говорит " << name(order.front()) << ".\n";
    // По одному: каждый следующий видит в снимке реплики предыдущих.
    for (PlayerId speaker : order) {
        SpeechEvent e;
        e.speaker = speaker;
        if (state_->extra.silenced == speaker) {
            e.text = "🙂 🙃 😶 (жертва шутки громилы — сегодня только смайлики)";
        } else if (std::optional<Response> answer = ask(make_request(RequestKind::Speech, speaker))) {
            e.text = answer->speech;
            e.claim = checked_claim(answer->claim);
        }
        out_ << "  " << name(speaker) << ": " << in_guillemets(e.text) << "\n";
        record(e);
    }
}

std::string Host::names(const std::vector<PlayerId>& ids) const {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        out += (i == 0 ? "" : ", ") + name(ids[i]);
    }
    return out;
}

void Host::voting() {
    state_->phase = Phase::Voting;
    out_ << "Голосование: каждый называет, кого считает мафией.\n";

    // Туры голосования, пока не будет решения. Кандидатов в каждом
    // переголосовании строго меньше, чем в предыдущем туре, — цикл конечен.
    std::vector<PlayerId> candidates;  // пусто — выбирают из всех живых
    for (int round = 1;; ++round) {
        const VoteTally tally = collect_votes(round, candidates);
        print_tally(round, tally);
        const VoteDecision decision = decide_vote(tally);
        switch (decision.outcome) {
            case VoteOutcome::Nobody:
                out_ << "Ведущий: ни одного засчитанного голоса. Никто не выбывает.\n";
                record(AnnouncementEvent{"Голосование не состоялось."});
                return;
            case VoteOutcome::Execute:
                execute(decision.players.front());
                return;
            case VoteOutcome::Revote:
                if (executioner_turn(decision.players)) {
                    return;
                }
                out_ << "Ведущий: ничья — " << names(decision.players) << " набрали по " << votes_text(tally.top)
                     << ". Переголосование между ними.\n";
                record(AnnouncementEvent{"Ничья: переголосование между " + names(decision.players) + "."});
                candidates = decision.players;
                continue;
            case VoteOutcome::Poll:
                if (executioner_turn(decision.players)) {
                    return;
                }
                kick_all_poll(decision.players, tally.top);
                return;
        }
        return;  // недостижимо: все исходы обработаны выше
    }
}

VoteTally Host::collect_votes(int round, const std::vector<PlayerId>& candidates) {
    if (round > 1) {
        out_ << "Переголосование, тур " << round << ": выбор между " << names(candidates) << ".\n";
    }
    // Голосующие и вес голоса (в полуголосах): живые не в тюрьме — 2;
    // при живом и не раскрывшемся некроманте — ещё выбывшие мирные, по 1.
    std::vector<std::pair<PlayerId, int>> voters;
    for (PlayerId v : speaking_order()) {
        if (!jailed(v)) {
            voters.emplace_back(v, 2);
        }
    }
    const std::optional<PlayerId> necro = find_role(RoleKind::Necromancer);
    if (necro && !state_->extra.necromancer_revealed) {
        for (const PlayerState& p : state_->players) {
            if (!p.alive && p.disclosed && !p.disclosed->is_mafia) {
                voters.emplace_back(p.id, 1);
            }
        }
    }

    std::vector<std::optional<PlayerId>> ballots;
    std::vector<int> weights;
    decisive_votes_.clear();
    // Последовательно, в том же порядке круга: голос виден следующим.
    for (const auto& [voter, weight] : voters) {
        VoteEvent e;  // target по умолчанию пуст — «голос не засчитан»
        e.voter = voter;
        e.round = round;
        if (state_->extra.silenced == voter) {
            out_ << "  " << name(voter) << " голосует смайликом — голос не считается\n";
            record(e);
            ballots.push_back(std::nullopt);
            weights.push_back(weight);
            continue;
        }
        Request request = make_request(RequestKind::Vote, voter);
        if (candidates.empty()) {
            request.targets = alive_except(voter);
        } else {
            // Против себя нельзя и в переголосовании: лидер выбирает из остальных лидеров.
            std::ranges::copy_if(candidates, std::back_inserter(request.targets),
                                 [voter](PlayerId c) { return c != voter; });
        }
        // Жертву шутки громилы сегодня казнить нельзя.
        std::erase_if(request.targets, [&](PlayerId c) { return state_->extra.silenced == c; });
        if (request.targets.empty()) {
            continue;
        }
        request.can_abstain = false;  // мирные не могут пропустить голосование
        if (std::optional<Response> answer = ask(request)) {
            e.target = answer->target;
        }
        const char* half = weight == 1 ? " (полголоса из-за некроманта)" : "";
        if (e.target) {
            out_ << "  " << name(voter) << half << " голосует против: " << name(*e.target) << "\n";
        } else {
            out_ << "  " << name(voter) << ": голос не засчитан\n";
        }
        record(e);
        decisive_votes_.push_back(e);
        // Заморозка босса: голос объявлен, но ведущий его молча не считает.
        const bool counted = !frozen(voter);
        if (!counted && e.target) {
            secret("голос " + name(voter) + " заморожен боссом — не учтён.");
        }
        ballots.push_back(counted ? e.target : std::nullopt);
        weights.push_back(weight);
    }
    return tally_votes(ballots, weights);
}

void Host::print_tally(int round, const VoteTally& tally) {
    out_ << "Итоги тура " << round << ":";
    for (const auto& [id, halves] : tally.counts) {
        out_ << " " << name(id) << " — " << votes_text(halves) << ";";
    }
    if (tally.spoiled > 0) {
        out_ << " не засчитано — " << tally.spoiled << ";";
    }
    out_ << "\n";
}

void Host::kick_all_poll(const std::vector<PlayerId>& leaders, int votes_each) {
    std::vector<PlayerId> voters = speaking_order();
    std::erase_if(voters, [this](PlayerId v) { return jailed(v); });  // заключённые не голосуют
    out_ << "Ведущий: все кандидаты набрали поровну — " << names(leaders) << " (по " << votes_text(votes_each)
         << "). Выгнать всех разом? Нужно «да» больше чем от половины живых: от "
         << voters.size() / 2 + 1 << " из " << voters.size() << ".\n";
    record(AnnouncementEvent{"Опрос: выгнать всех разом — " + names(leaders) + "?"});

    int yes = 0;
    for (PlayerId voter : voters) {
        Request request = make_request(RequestKind::Choice, voter);
        request.options = {"да", "нет"};
        request.targets = leaders;  // о ком вопрос — чтобы ответить осознанно
        std::optional<Response> answer = ask(request);
        PollEvent e;
        e.voter = voter;
        e.agree = answer && answer->choice == std::size_t{0};
        out_ << "  " << name(voter) << ": " << (e.agree ? "да" : "нет") << "\n";
        record(e);
        yes += e.agree ? 1 : 0;
    }

    const int total = static_cast<int>(voters.size());
    if (kick_all_passes(yes, total)) {
        out_ << "Ведущий: «да» — " << yes << " из " << total << ". Город выгоняет всех: " << names(leaders)
             << ".\n";
        for (PlayerId id : leaders) {
            execute(id);
        }
    } else {
        out_ << "Ведущий: «да» — " << yes << " из " << total << ", этого мало. Никто не выбывает.\n";
        record(AnnouncementEvent{"Опрос не прошёл, никто не выбывает."});
    }
}

Disclosure Host::disclose(const PlayerState& p) const {
    Disclosure d;
    d.is_mafia = team_of(p.role) == Team::Mafia;
    if (config_.open_announcements) {
        d.role = p.role;  // открытое объявление: полный ролевой статус
    }
    return d;
}

std::string Host::disclosure_text(const Disclosure& d) {
    if (d.role) {
        return "Это был: " + std::string(role_name(*d.role)) + " (" + std::string(team_name(team_of(*d.role))) + ").";
    }
    return d.is_mafia ? "Это был мафиози." : "Это был не мафиози.";
}

void Host::execute(PlayerId victim) {
    PlayerState& p = state_->players[static_cast<std::size_t>(victim)];
    if (p.role == RoleKind::Elder) {
        // Старейшину нельзя казнить; ведущий объявляет это — и тем раскрывает его.
        Disclosure d;
        d.role = RoleKind::Elder;
        p.disclosed = d;
        record(RevealEvent{victim, d});
        out_ << "Ведущий: " << p.name << " — старейшина, его нельзя казнить. Никто не выбывает.\n";
        return;
    }
    p.alive = false;
    const Disclosure d = disclose(p);
    p.disclosed = d;

    DeathEvent e;
    e.victim = victim;
    e.cause = DeathCause::Executed;
    e.disclosed = d;
    record(e);
    out_ << "Ведущий: по итогам голосования город покидает " << p.name << ". " << disclosure_text(d) << "\n";

    std::optional<Response> last = ask(make_request(RequestKind::LastWord, victim));
    LastWordEvent w;
    w.speaker = victim;
    if (last) {
        w.text = last->speech;
        w.claim = checked_claim(last->claim);
    }
    out_ << "  Последнее слово — " << p.name << ": " << in_guillemets(w.text) << "\n";
    record(w);

    if (p.role == RoleKind::Blade) {
        // Казнённый блейд забирает с собой того, чей голос против него был первым.
        auto first = std::ranges::find_if(decisive_votes_, [victim](const VoteEvent& v) { return v.target == victim; });
        if (first != decisive_votes_.end() && is_alive(first->voter)) {
            kill_now(first->voter, "Ведущий: " + p.name + " был блейдом и забирает с собой " + name(first->voter) +
                                       " — первого, кто голосовал против него.");
        }
    }
    after_deaths({victim});
}

void Host::necromancer_turn() {
    const std::optional<PlayerId> necro = find_role(RoleKind::Necromancer);
    if (!necro || state_->extra.necromancer_revealed || jailed(*necro)) {
        return;
    }
    std::vector<PlayerId> dead;
    for (const PlayerState& p : state_->players) {
        if (!p.alive && !(p.disclosed && p.disclosed->is_mafia)) {
            dead.push_back(p.id);
        }
    }
    if (dead.empty()) {
        return;
    }
    Request r = make_request(RequestKind::Choice, *necro);
    r.choice = ChoiceKind::Revive;
    r.targets = dead;
    for (PlayerId d : dead) {
        r.options.push_back(name(d));
    }
    r.options.push_back("не сейчас");
    const std::optional<Response> answer = ask(r);
    if (!answer || !answer->choice || *answer->choice >= dead.size()) {
        return;
    }
    const PlayerId target = dead[*answer->choice];
    PlayerState& n = state_->players[static_cast<std::size_t>(*necro)];
    state_->players[static_cast<std::size_t>(target)].alive = true;
    state_->extra.necromancer_revealed = true;  // выбывшие мирные больше не голосуют
    n.role = RoleKind::Civilian;                // некромант теряет статус
    Disclosure d;
    d.role = RoleKind::Necromancer;
    n.disclosed = d;
    record(RevealEvent{*necro, d});
    record(ReviveEvent{target});
    out_ << "Ведущий: " << n.name << " раскрывается — он некромант — и воскрешает " << name(target)
         << ". Отныне он обычный мирный.\n";
}

bool Host::executioner_turn(const std::vector<PlayerId>& leaders) {
    const std::optional<PlayerId> ex = find_role(RoleKind::Executioner);
    if (!ex || jailed(*ex)) {
        return false;
    }
    Request r = make_request(RequestKind::Choice, *ex);
    r.choice = ChoiceKind::Execute;
    r.targets = leaders;
    for (PlayerId l : leaders) {
        r.options.push_back(name(l));
    }
    r.options.push_back("помиловать");
    const std::optional<Response> answer = ask(r);
    if (!answer || !answer->choice || *answer->choice >= leaders.size()) {
        out_ << "Ведущий: палач никого не приговорил.\n";
        record(AnnouncementEvent{"Палач милует."});
        return false;
    }
    const PlayerId victim = leaders[*answer->choice];
    out_ << "Ведущий: ничья — палач выносит приговор: " << name(victim) << ".\n";
    record(AnnouncementEvent{"Палач приговорил " + name(victim) + "."});
    execute(victim);
    return true;
}

void Host::kill_now(PlayerId victim, const std::string& announcement) {
    PlayerState& p = state_->players[static_cast<std::size_t>(victim)];
    if (!p.alive) {
        return;
    }
    p.alive = false;
    const Disclosure d = disclose(p);
    p.disclosed = d;
    DeathEvent e;
    e.victim = victim;
    e.cause = DeathCause::Revenge;
    e.disclosed = d;
    record(e);
    out_ << announcement << " Выбывает " << p.name << ". " << disclosure_text(d) << "\n";
    std::optional<Response> last = ask(make_request(RequestKind::LastWord, victim));
    LastWordEvent w;
    w.speaker = victim;
    if (last) {
        w.text = last->speech;
        w.claim = checked_claim(last->claim);
    }
    out_ << "  Последнее слово — " << p.name << ": " << in_guillemets(w.text) << "\n";
    record(w);
    after_deaths({victim});
}

void Host::after_deaths(const std::vector<PlayerId>& died) {
    for (PlayerId d : died) {
        PlayerState& p = state_->players[static_cast<std::size_t>(d)];
        switch (p.role) {
            case RoleKind::Cattani: {
                // Погибая, Каттани успевает выстрелить в любого.
                Request r = make_request(RequestKind::NightMove, d);
                r.actions = {NightActionKind::Shoot};
                r.targets = alive_except(d);
                r.can_abstain = true;
                if (r.targets.empty()) {
                    break;
                }
                const std::optional<Response> a = ask(r);
                if (a && a->night.kind == NightActionKind::Shoot && a->night.target && is_alive(*a->night.target)) {
                    kill_now(*a->night.target, "Ведущий: " + p.name + " — комиссар Каттани — успевает выстрелить.");
                }
                break;
            }
            case RoleKind::Necromancer: {
                // После смерти некроманта его статус объявляется всем.
                Disclosure dis = p.disclosed.value_or(Disclosure{});
                dis.role = RoleKind::Necromancer;
                p.disclosed = dis;
                state_->extra.necromancer_revealed = true;
                record(RevealEvent{d, dis});
                out_ << "Ведущий: " << p.name << " был некромантом — выбывшие больше не голосуют.\n";
                break;
            }
            case RoleKind::Jailer:
                if (!find_role(RoleKind::Jailer) && !state_->extra.jailed.empty()) {
                    out_ << "Ведущий: тюремщик выбыл — заключённые " << names({state_->extra.jailed.begin(), state_->extra.jailed.end()})
                         << " на свободе.\n";
                    record(AnnouncementEvent{"Заключённые освобождены."});
                    state_->extra.jailed.clear();
                }
                break;
            default:
                break;
        }
    }
    transform_roles(died);
    last_boss_ = boss();
}

void Host::transform_roles(const std::vector<PlayerId>& died) {
    auto alive_with = [this](auto pred) {
        std::vector<PlayerId> out;
        for (const PlayerState& p : state_->players) {
            if (p.alive && pred(p)) {
                out.push_back(p.id);
            }
        }
        return out;
    };
    auto become = [this](PlayerId id, RoleKind role, const std::string& why) {
        state_->players[static_cast<std::size_t>(id)].role = role;
        tell(id, why);
        secret(name(id) + " теперь " + std::string(role_name(role)) + ": " + why);
    };
    // Комиссар выбыл — сержант становится комиссаром.
    if (!find_role(RoleKind::Commissar)) {
        for (PlayerId s : alive_with([](const PlayerState& p) { return p.role == RoleKind::Sergeant; })) {
            become(s, RoleKind::Commissar, "комиссар выбыл — теперь вы комиссар.");
        }
    }
    // Выбыл Босс — подкидыш становится мафиози.
    if (last_boss_ && std::ranges::find(died, *last_boss_) != died.end()) {
        for (PlayerId f : alive_with([](const PlayerState& p) { return p.role == RoleKind::Foundling; })) {
            become(f, RoleKind::Mafioso, "Босс выбыл — вы узнаёте, что вы его сын, и становитесь мафиози.");
        }
    }
    // Нет Босса и «ранговых» мафиози — похититель становится Боссом.
    const auto ranked = alive_with([](const PlayerState& p) {
        return team_of(p.role) == Team::Mafia && p.role != RoleKind::Kidnapper && p.role != RoleKind::Werewolf;
    });
    if (ranked.empty()) {
        for (PlayerId k : alive_with([](const PlayerState& p) { return p.role == RoleKind::Kidnapper; })) {
            become(k, RoleKind::Boss, "из мафии остались не все — теперь вы Босс.");
        }
    }
    // Последний мафиози выбыл — оборотень получает пистолет.
    const auto mafia = alive_with([](const PlayerState& p) {
        return team_of(p.role) == Team::Mafia && p.role != RoleKind::Werewolf;
    });
    if (mafia.empty()) {
        for (PlayerId w : alive_with([](const PlayerState& p) { return p.role == RoleKind::Werewolf; })) {
            become(w, RoleKind::Mafioso, "последний мафиози выбыл — теперь вы полноправный мафиози.");
        }
    }
}

// ---------------------------------------------------------------------------
// Ночь
// ---------------------------------------------------------------------------

int Host::alive_count() const {
    // Проекция &PlayerState::alive: алгоритм сам достаёт поле, лямбда не нужна.
    return static_cast<int>(std::ranges::count_if(state_->players, std::identity{}, &PlayerState::alive));
}

bool Host::jailed(PlayerId id) const {
    return state_->extra.jailed.contains(id);
}

bool Host::frozen(PlayerId id) const {
    return state_->extra.frozen == id;
}

std::optional<PlayerId> Host::find_role(RoleKind role) const {
    auto it = std::ranges::find_if(state_->players, [role](const PlayerState& p) { return p.alive && p.role == role; });
    return it != state_->players.end() ? std::optional<PlayerId>(it->id) : std::nullopt;
}

void Host::tell(PlayerId to, const KnownIdentity& fact) {
    state_->extra.facts[to].push_back(fact);
}

void Host::tell(PlayerId to, const std::string& note) {
    state_->extra.notes[to].push_back(note);
}

std::optional<PlayerId> Host::boss() const {
    // Босс мафии, если он в партии и жив; иначе — первый живой мафиози по
    // рассадке (спящий оборотень не в счёт). find_if останавливается на первом.
    if (std::optional<PlayerId> b = find_role(RoleKind::Boss)) {
        return b;
    }
    auto it = std::ranges::find_if(state_->players, [](const PlayerState& p) {
        return p.alive && team_of(p.role) == Team::Mafia && !role_capabilities(p.role).dormant;
    });
    return it != state_->players.end() ? std::optional<PlayerId>(it->id) : std::nullopt;
}

std::vector<PlayerId> Host::night_targets(PlayerId actor, NightActionKind action) const {
    const RoleKind role = state_->players[static_cast<std::size_t>(actor)].role;
    const bool mafia = team_of(role) == Team::Mafia;
    // Правило — один предикат; отбор и номера — общая ленивая цепочка.
    auto allowed = [&](const PlayerState& p) {
        switch (action) {
            case NightActionKind::Heal:
                return p.alive && Doctor::may_protect(*state_, actor, p.id);  // правило — по журналу
            case NightActionKind::Revive: {
                const auto& done = state_->extra.revived;
                return !p.alive && !(done.contains(actor) && done.at(actor).contains(p.id));
            }
            case NightActionKind::Prank: {
                const auto& done = state_->extra.pranked;  // над собой можно, над каждым — раз
                return p.alive && !(done.contains(actor) && done.at(actor).contains(p.id));
            }
            default:
                break;
        }
        if (!p.alive || p.id == actor) {
            return false;
        }
        if (mafia) {
            // Своих мафия не выбирает; подкидыша она знает и тронуть не может;
            // вор блокирует только мирных.
            return team_of(p.role) != Team::Mafia && p.role != RoleKind::Foundling &&
                   (action != NightActionKind::Block || team_of(p.role) == Team::Civilians);
        }
        return true;
    };
    auto ids = state_->players | std::views::filter(allowed) | std::views::transform(&PlayerState::id);
    return {std::ranges::begin(ids), std::ranges::end(ids)};
}

void Host::secret(const std::string& line) {
    // Только в журнал (и только в полный). В out_ не пишем никогда: out_ —
    // это ещё и консоль.
    if (log_) {
        out_.flush();  // сохранить порядок строк в файле круга
        log_->secret(line);
    }
}

bool Host::extra_valid(const Request& request, const Response& response) const {
    const NightAction& a = response.night;
    switch (a.kind) {
        case NightActionKind::Compare:
            return a.second && a.second != a.target && std::ranges::find(request.targets, *a.second) != request.targets.end();
        case NightActionKind::Kidnap:
            return a.guess.has_value();
        case NightActionKind::Antidote:
            // Противоядие — только тому, кого сам отравил прошлой ночью.
            return std::ranges::any_of(state_->extra.poisonings, [&](const Poisoning& p) {
                return p.by == request.you.self && p.target == a.target && p.night == state_->day - 1 && !p.neutralized;
            });
        default:
            return true;
    }
}

std::vector<NightOrder> Host::mafia_council() {
    // «Совет мафии»: каждый член мафии голосует за общую жертву.
    std::vector<Request> requests;
    for (const PlayerState& p : state_->players) {
        if (p.alive && team_of(p.role) == Team::Mafia && !role_capabilities(p.role).dormant && !jailed(p.id) &&
            !frozen(p.id)) {
            Request r = make_request(RequestKind::NightMove, p.id);
            r.targets = night_targets(p.id, NightActionKind::Kill);
            r.actions = {NightActionKind::Kill};
            if (!r.targets.empty()) {
                requests.push_back(std::move(r));
            }
        }
    }
    if (requests.empty()) {
        return {};
    }
    const std::vector<Response> answers = engine_->exchange(requests);
    std::vector<std::pair<PlayerId, PlayerId>> votes;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        std::optional<Response> a = is_valid_answer(requests[i], answers[i]) ? std::optional(answers[i]) : ask(requests[i]);
        if (a && a->night.kind == NightActionKind::Kill && a->night.target) {
            votes.emplace_back(requests[i].you.self, *a->night.target);
            state_->night_log.push_back(NightRecord{state_->day, requests[i].you.self, a->night});
            secret("мафиози " + name(requests[i].you.self) + " предлагает: " + name(*a->night.target) + ".");
        }
    }
    if (votes.empty()) {
        return {};
    }
    // Исполнитель — Босс; если он сегодня не на совете (в тюрьме, заморожен) — первый проголосовавший.
    PlayerId executor = votes.front().first;
    if (std::optional<PlayerId> b = boss(); b && std::ranges::any_of(votes, [&](const auto& v) { return v.first == *b; })) {
        executor = *b;
    }
    MafiaDecision decision = decide_mafia(votes, executor);
    if (!decision.victim && !decision.tied.empty()) {
        Request r = make_request(RequestKind::NightMove, executor);
        r.targets = decision.tied;
        r.actions = {NightActionKind::Kill};
        const std::optional<Response> a = ask(r);
        decision.victim = a ? a->night.target : decision.tied.front();
    }
    if (!decision.victim) {
        return {};
    }
    secret("решение мафии (исполняет " + name(executor) + "): " + name(*decision.victim) + ".");
    NightOrder o;
    o.actor = executor;
    o.role = state_->players[static_cast<std::size_t>(executor)].role;
    o.action.kind = NightActionKind::Kill;
    o.action.target = decision.victim;
    o.council = true;
    return {o};
}

std::vector<NightOrder> Host::abilities() {
    // Особые ночные способности всех ролей — одним пакетом.
    std::vector<Request> requests;
    const auto alive_mafia = std::ranges::count_if(state_->players, [](const PlayerState& p) {
        return p.alive && team_of(p.role) == Team::Mafia && !role_capabilities(p.role).dormant;
    });
    for (const PlayerState& p : state_->players) {
        if (!p.alive || jailed(p.id) || frozen(p.id)) {
            continue;
        }
        const RoleCapabilities caps = role_capabilities(p.role);
        std::vector<NightActionKind> actions;
        if (caps.night_actor && !caps.dormant) {
            for (NightActionKind a : caps.night_actions) {
                if (team_of(p.role) == Team::Mafia && a == NightActionKind::Kill && p.role != RoleKind::Killer) {
                    continue;  // голос мафии — на совете, не здесь
                }
                if (p.role == RoleKind::Advocate && a == NightActionKind::Shoot && alive_mafia > 1) {
                    continue;  // стрелять адвокат может, лишь оставшись единственным мафиози
                }
                actions.push_back(a);
            }
        }
        bool granted = false;
        if (actions.empty() && state_->extra.granted_check.contains(p.id)) {
            actions = {NightActionKind::Check};  // дар донора: одна проверка
            granted = true;
        }
        if (actions.empty() || state_->extra.fan_done.contains(p.id)) {
            continue;
        }
        if (p.role == RoleKind::Drunk && state_->extra.drunk_streak[p.id] >= 2) {
            secret(p.name + " (пьяница) третью ночь подряд в гости не идёт.");
            continue;
        }
        Request r = make_request(RequestKind::NightMove, p.id);
        r.targets = night_targets(p.id, actions.front());
        r.actions = actions;
        r.can_abstain = caps.may_skip_night || granted;
        const std::size_t need = actions.front() == NightActionKind::Compare ? 2 : 1;
        if (r.targets.size() < need) {
            secret(p.name + " (" + std::string(role_name(p.role)) + ") — некого выбрать, ход пропущен.");
            continue;
        }
        requests.push_back(std::move(r));
    }
    state_->extra.granted_check.clear();

    std::vector<NightOrder> orders;
    if (requests.empty()) {
        return orders;
    }
    const std::vector<Response> answers = engine_->exchange(requests);
    std::set<PlayerId> visited_drunks;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const Request& req = requests[i];
        std::optional<Response> a;
        if (is_valid_answer(req, answers[i]) && extra_valid(req, answers[i])) {
            a = answers[i];
        } else if (std::optional<Response> again = ask(req); again && extra_valid(req, *again)) {
            a = again;
        }
        const PlayerId actor = req.you.self;
        if (!a || a->night.kind == NightActionKind::None) {
            secret(name(actor) + " — хода нет.");
            continue;
        }
        NightOrder o;
        o.actor = actor;
        o.role = state_->players[static_cast<std::size_t>(actor)].role;
        o.action = a->night;
        orders.push_back(o);
        state_->night_log.push_back(NightRecord{state_->day, actor, a->night});  // тайный журнал
        const PlayerId t = *a->night.target;
        switch (a->night.kind) {
            case NightActionKind::Prank:
                state_->extra.pranked[actor].insert(t);
                break;
            case NightActionKind::Revive:
                state_->extra.revived[actor].insert(t);
                break;
            case NightActionKind::Visit:
                visited_drunks.insert(actor);
                break;
            default:
                break;
        }
    }
    for (const PlayerState& p : state_->players) {
        if (p.role == RoleKind::Drunk) {
            int& streak = state_->extra.drunk_streak[p.id];
            streak = visited_drunks.contains(p.id) ? streak + 1 : 0;
        }
    }
    return orders;
}

void Host::run_night() {
    if (!started_) {
        throw std::logic_error("Host::run_night: партия не подготовлена");
    }
    if (state_->finished) {
        return;
    }
    state_->phase = Phase::Night;
    out_ << "\n=== Ночь " << state_->day << " ===\nВедущий: город засыпает. Просыпаются те, у кого есть ночной ход.\n";

    // 1. Собрать ВСЕ ночные ходы: сначала совет мафии, затем способности.
    //    Ни один ход не применяется, пока не собраны оба пакета.
    last_boss_ = boss();
    std::vector<NightOrder> orders = mafia_council();
    std::vector<NightOrder> more = abilities();
    orders.insert(orders.end(), more.begin(), more.end());

    // 2. Разобрать ночь целиком — чистой функцией (game/night_rules.hpp).
    const NightReport report = resolve_full_night(*state_, orders);
    for (const std::string& s : report.secrets) {
        secret(s);
    }

    // 3. Применить итог: смерти и раскрытия — утром, остальное — в тайное состояние.
    ExtraState& x = state_->extra;
    pending_deaths_ = report.deaths;
    pending_killers_ = {report.killed_by.begin(), report.killed_by.end()};  // нужны только открытым объявлениям
    pending_saved_ = report.saved;
    x.poisonings = report.poisonings;
    for (const auto& [actor, added] : report.honest_kills) {
        x.honest_kills[actor] += added;
    }
    for (PlayerId j : report.jailed) {
        x.jailed.insert(j);
        pending_jail_.push_back(j);
    }
    x.pending_revive = report.revived;
    x.pending_silence = report.prank;
    x.pending_freeze = report.freeze;
    x.granted_check.insert(report.granted_check.begin(), report.granted_check.end());
    x.fan_done.insert(report.fan_done.begin(), report.fan_done.end());
    for (const auto& [to, fact] : report.facts) {
        tell(to, fact);
    }
    for (const auto& [to, note] : report.notes) {
        tell(to, note);
    }
    pending_reveals_ = report.reveals;
    secret(pending_deaths_.empty() ? std::string("итог: смертей нет.")
                                   : "итог: утром выбывают — " + names(pending_deaths_) + ".");
    out_ << "Ведущий: ночь окончена. Итоги — утром.\n";
}

void Host::morning() {
    if (state_->day == 1) {
        return;  // перед первым днём ночи не было
    }
    state_->phase = Phase::Morning;
    ExtraState& x = state_->extra;
    x.silenced = std::exchange(x.pending_silence, std::nullopt);  // шутка — на сегодняшний день
    x.frozen = std::exchange(x.pending_freeze, std::nullopt);     // заморозка — на эти сутки

    // Закрытое объявление: кто выбыл и мафия ли он; кто убил и было ли
    // спасение — тайна. Открытое: ещё роль выбывшего, кто его убил (роль,
    // не номер) и на кого напали, но спасли.
    std::vector<PlayerId> died;
    for (PlayerId v : pending_deaths_) {
        PlayerState& p = state_->players[static_cast<std::size_t>(v)];
        if (!p.alive) {
            continue;
        }
        p.alive = false;
        const Disclosure d = disclose(p);
        p.disclosed = d;
        DeathEvent e;
        e.victim = v;
        e.cause = DeathCause::Night;
        e.disclosed = d;
        if (config_.open_announcements) {
            if (auto it = pending_killers_.find(v); it != pending_killers_.end()) {
                e.killed_by = it->second;
            }
        }
        record(e);
        out_ << "Утро. Ведущий: этой ночью выбыл " << p.name << ". " << disclosure_text(d);
        if (e.killed_by) {
            out_ << " Убит " << killer_phrase(*e.killed_by) << ".";
        }
        out_ << "\n";
        died.push_back(v);
    }
    pending_deaths_.clear();
    pending_killers_.clear();
    if (died.empty()) {
        out_ << "Утро. Ведущий: этой ночью никто не погиб.\n";
        record(AnnouncementEvent{"Ночь прошла без смертей."});
    }
    if (config_.open_announcements) {
        for (PlayerId s : pending_saved_) {
            if (is_alive(s)) {
                out_ << "Ведущий: ночью на " << name(s) << " напали, но его спасли.\n";
                record(SavedEvent{s});
            }
        }
    }
    pending_saved_.clear();
    for (PlayerId t : x.pending_revive) {
        PlayerState& p = state_->players[static_cast<std::size_t>(t)];
        if (!p.alive && std::ranges::find(died, t) == died.end()) {
            p.alive = true;
            record(ReviveEvent{t});
            out_ << "Ведущий: реаниматор вернул в игру " << p.name << ".\n";
        }
    }
    x.pending_revive.clear();
    for (PlayerId j : pending_jail_) {
        if (is_alive(j)) {
            out_ << "Ведущий: " << name(j) << " в тюрьме — ночью не ходит и не голосует.\n";
            record(AnnouncementEvent{name(j) + " в тюрьме."});
        }
    }
    pending_jail_.clear();
    for (const auto& [t, d] : pending_reveals_) {
        PlayerState& p = state_->players[static_cast<std::size_t>(t)];
        if (p.alive) {
            p.disclosed = d;  // хакер: результат взлома — всем
            record(RevealEvent{t, d});
            out_ << "Ведущий: хакер взломал " << p.name << " — это " << role_name(*d.role) << ".\n";
        }
    }
    pending_reveals_.clear();
    if (x.silenced && is_alive(*x.silenced)) {
        out_ << "Ведущий: сегодня " << name(*x.silenced)
             << " — жертва шутки громилы: говорит только смайликами, не голосует, казнить его нельзя.\n";
        record(AnnouncementEvent{name(*x.silenced) + " — жертва шутки громилы."});
    }
    for (PlayerId v : died) {
        std::optional<Response> last = ask(make_request(RequestKind::LastWord, v));
        LastWordEvent w;
        w.speaker = v;
        if (last) {
            w.text = last->speech;
            w.claim = checked_claim(last->claim);
        }
        out_ << "  Последнее слово — " << name(v) << ": " << in_guillemets(w.text) << "\n";
        record(w);
    }
    after_deaths(died);
}

// ---------------------------------------------------------------------------
// Запросы
// ---------------------------------------------------------------------------

SharedPtr<const PublicSnapshot> Host::snapshot() const {
    auto s = make_shared_ptr<PublicSnapshot>();
    s->day = state_->day;
    s->phase = state_->phase;
    s->mafia_total = mafia_count(static_cast<int>(state_->players.size()), config_.mafia_divisor);
    s->players.reserve(state_->players.size());
    std::ranges::transform(state_->players, std::back_inserter(s->players), [](const PlayerState& p) {
        PublicPlayer pub;  // роли здесь нет — по построению
        pub.id = p.id;
        pub.name = p.name;
        pub.alive = p.alive;
        pub.disclosed = p.disclosed;
        return pub;
    });
    s->history = state_->history;
    return s;
}

Request Host::make_request(RequestKind kind, PlayerId to) {
    Request r;
    r.id = next_request_id_++;
    r.kind = kind;
    r.world = snapshot();
    r.you = briefing_for(to);
    return r;
}

std::optional<Response> Host::ask(Request request) {
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        std::vector<Response> answers = engine_->exchange({request});
        if (!answers.empty() && is_valid_answer(request, answers.front())) {
            return answers.front();
        }
        out_ << "  Ведущий: недопустимый ответ от " << name(request.you.self) << " (попытка " << attempt
             << " из " << kMaxAttempts << ").\n";
        request.id = next_request_id_++;  // новый номер: старый ответ не спутать с новым
    }
    return std::nullopt;
}

void Host::shutdown() {
    if (!started_) {
        return;
    }
    std::vector<Request> stops;
    for (const PlayerState& p : state_->players) {
        stops.push_back(make_request(RequestKind::Stop, p.id));
    }
    state_->phase = Phase::Finished;
    engine_->exchange(std::move(stops));
    engine_->stop();
    started_ = false;
}

// ---------------------------------------------------------------------------
// Конец игры
// ---------------------------------------------------------------------------

void Host::play() {
    if (!started_) {
        throw std::logic_error("Host::play: партия не подготовлена");
    }
    while (!state_->finished) {
        run_day();
        if (state_->finished) {
            break;
        }
        // Предел — после дня: ночь, смерти которой некому было бы объявить, не начинаем.
        if (state_->day >= config_.max_days) {
            finish(Verdict::Draw, "сыграно " + std::to_string(config_.max_days) + " дней — предел партии",
                   "day_limit");
            break;
        }
        run_night();
    }
    print_summary();
}

bool Host::check_end() {
    const AliveCount a = count_alive(*state_);
    const Verdict v = judge(a);
    switch (v) {
        case Verdict::Continue:
            return false;
        case Verdict::CiviliansWin:
            finish(v, "мафия и одиночки-убийцы выбыли", "all_threats_out");
            break;
        case Verdict::MafiaMajority:
            finish(v, "мафии больше, чем всех остальных", "mafia_majority");
            break;
        case Verdict::MafiaParity:
            finish(v, "мафии столько же, сколько остальных, и одиночек-убийц нет", "mafia_parity");
            break;
        case Verdict::ManiacWin:
            finish(v, a.civilians == 1 ? "одиночка-убийца остался один на один с мирным" : "одиночка-убийца остался один",
                   "maniac_duel");
            break;
        case Verdict::Draw:
            finish(v, "за столом не осталось никого", "all_dead");
            break;
    }
    return true;
}

void Host::finish(Verdict verdict, std::string reason, std::string code) {
    state_->finished = true;
    state_->winner = winner_of(verdict);
    state_->phase = Phase::Finished;
    end_reason_ = std::move(reason);
    end_code_ = std::move(code);
    out_ << "\nВедущий: игра окончена — " << end_reason_ << ". ";
    if (state_->winner) {
        out_ << "Победа: " << team_name(*state_->winner) << "!\n";
    } else {
        out_ << "Ничья.\n";
    }
    record(AnnouncementEvent{"Игра окончена: " + end_reason_ + "."});
}

namespace {

// Ширина строки в символах (UTF-8): кириллица занимает 2 байта, а std::setw
// считает байты — таблица бы «поехала».
std::size_t display_width(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        n += (c & 0xC0) != 0x80 ? 1 : 0;  // продолжающие байты не считаем
    }
    return n;
}

std::string pad(const std::string& s, std::size_t width) {
    const std::size_t w = display_width(s);
    return w >= width ? s : s + std::string(width - w, ' ');
}

}  // namespace

std::string Host::summary_table() const {
    // Как выбыл каждый — из публичной истории: казнь днём или смерть ночью
    // (ночная смерть записана утром следующего дня).
    std::vector<std::string> fate(state_->players.size(), "жив");
    for (const HistoryEntry& h : state_->history) {
        if (const auto* d = std::get_if<DeathEvent>(&h.event)) {
            std::string& f = fate[static_cast<std::size_t>(d->victim)];
            switch (d->cause) {
                case DeathCause::Executed:
                    f = "казнён днём " + std::to_string(h.day);
                    break;
                case DeathCause::Night:
                    f = "убит ночью " + std::to_string(h.day - 1);
                    break;
                case DeathCause::Revenge:
                    f = "забран с собой (день " + std::to_string(h.day) + ")";
                    break;
            }
        } else if (const auto* r = std::get_if<ReviveEvent>(&h.event)) {
            fate[static_cast<std::size_t>(r->target)] = "жив (воскрешён)";
        }
    }

    const std::vector<std::string> head = {"Игрок", "Роль", "Команда", "Исход"};
    std::vector<std::vector<std::string>> rows;
    for (const PlayerState& p : state_->players) {
        rows.push_back({p.name, std::string(role_name(p.role)), std::string(team_name(team_of(p.role))),
                        fate[static_cast<std::size_t>(p.id)]});
    }
    // Ширина колонки — максимум по заголовку и всем строкам (ленивая цепочка,
    // без промежуточного вектора ширин).
    std::vector<std::size_t> width(head.size());
    for (std::size_t c = 0; c < head.size(); ++c) {
        auto widths = rows | std::views::transform([c](const auto& row) { return display_width(row[c]); });
        width[c] = std::max(display_width(head[c]), std::ranges::max(widths));
    }

    std::ostringstream out;
    auto print_row = [&](const std::vector<std::string>& row) {
        out << " ";
        for (std::size_t c = 0; c < row.size(); ++c) {
            const bool last = c + 1 == row.size();
            out << " " << (last ? row[c] : pad(row[c], width[c]) + " |");
        }
        out << "\n";
    };
    print_row(head);
    std::size_t total = 0;
    for (std::size_t w : width) {
        total += w + 3;
    }
    out << "  " << std::string(total - 2, '-') << "\n";
    for (const auto& row : rows) {
        print_row(row);
    }
    return out.str();
}

std::string Host::player_statistics() const {
    // Статистика по каждому игроку — подсчёты по публичной истории.
    std::ostringstream out;
    out << "Статистика по игрокам (из публичной истории):\n";
    for (const PlayerState& p : state_->players) {
        const PlayerId id = p.id;
        auto votes = state_->history | std::views::transform(&HistoryEntry::event) |
                     std::views::filter([](const Event& e) { return std::holds_alternative<VoteEvent>(e); }) |
                     std::views::transform([](const Event& e) { return std::get<VoteEvent>(e); });
        auto claims = state_->history | std::views::transform(&HistoryEntry::event) |
                      std::views::filter([](const Event& e) { return std::holds_alternative<SpeechEvent>(e); }) |
                      std::views::transform([](const Event& e) { return std::get<SpeechEvent>(e); }) |
                      std::views::filter([](const SpeechEvent& s) { return s.claim && s.claim->mafia; });

        const auto cast = std::ranges::count_if(votes, [id](const VoteEvent& v) { return v.voter == id && v.target; });
        const auto received = std::ranges::count_if(votes, [id](const VoteEvent& v) { return v.target == id; });
        const auto on_mafia = std::ranges::count_if(votes, [&](const VoteEvent& v) {
            return v.voter == id && v.target &&
                   team_of(state_->players[static_cast<std::size_t>(*v.target)].role) == Team::Mafia;
        });
        const auto accused = std::ranges::count_if(claims, [id](const SpeechEvent& s) { return s.speaker == id; });
        const auto accused_by = std::ranges::count_if(claims, [id](const SpeechEvent& s) { return s.claim->about == id; });

        out << "  " << p.name << " (" << role_name(p.role) << "): голосов отдано — " << cast
            << ", из них против мафии — " << on_mafia << "; голосов получено — " << received
            << "; обвинял — " << accused << " раз, обвиняли его — " << accused_by << " раз.\n";
    }
    return out.str();
}

std::string Host::result_line() const {
    // Одна машиночитаемая строка: только ASCII, пары ключ=значение, номера с 1.
    std::ostringstream out;
    out << "RESULT winner=" << (state_->finished ? winner_code(state_->winner) : "none")
        << " reason=" << (end_code_.empty() ? "unfinished" : end_code_) << " days=" << state_->day
        << " players=" << state_->players.size() << " seed=" << config_.seed << " survivors=";
    auto survivors = state_->players | std::views::filter(&PlayerState::alive) |
                     std::views::transform([](const PlayerState& p) { return p.id + 1; });
    if (std::ranges::empty(survivors)) {
        out << "none";
    }
    for (bool first = true; int seat : survivors) {
        out << (first ? "" : ",") << seat;
        first = false;
    }
    return out.str();
}

void Host::print_summary() {
    std::ostringstream text;
    text << "\n=== Итог партии ===\n";
    if (!state_->finished) {
        text << "Партия не доиграна.\n";
    } else if (state_->winner) {
        text << "Победитель: " << team_name(*state_->winner) << " (" << end_reason_ << ").\n";
    } else {
        text << "Ничья (" << end_reason_ << ").\n";
    }
    text << "Дней сыграно: " << state_->day << ".\n\n" << summary_table();
    const std::string result = result_line();
    text << "\n" << result << "\n";

    out_ << text.str();  // консоль и файл последнего круга (в тихом режиме — только файл)
    if (config_.quiet) {
        // В консоль — коротко: без таблицы на сотни строк, она в summary.txt.
        console_ << "\n";
        if (!state_->finished) {
            console_ << "Партия не доиграна.\n";
        } else if (state_->winner) {
            console_ << "Победитель: " << team_name(*state_->winner) << " (" << end_reason_ << ").\n";
        } else {
            console_ << "Ничья (" << end_reason_ << ").\n";
        }
        console_ << "Дней сыграно: " << state_->day << ", в живых: " << alive_count() << " из "
                 << state_->players.size() << ".\n"
                 << result << "\n";
        if (log_) {
            console_ << "Протокол и таблица игроков — в журнале: " << log_->dir().string() << "\n";
        }
    }
    if (log_) {
        out_.flush();
        log_->write_summary(text.str() + "\n" + player_statistics());
        log_->append_history(result + " dir=" + log_->dir().filename().string());
    }
}

std::optional<Claim> Host::checked_claim(const std::optional<Claim>& claim) const {
    if (claim && claim->about >= 0 && static_cast<std::size_t>(claim->about) < state_->players.size()) {
        return claim;
    }
    return std::nullopt;
}

const std::string& Host::name(PlayerId id) const {
    return state_->players.at(static_cast<std::size_t>(id)).name;
}

void Host::record(Event event) {
    HistoryEntry h;
    h.day = state_->day;
    h.phase = state_->phase;
    h.event = std::move(event);
    state_->history.push_back(std::move(h));
}

}  // namespace mafia
