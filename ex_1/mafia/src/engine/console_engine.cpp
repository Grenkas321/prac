#include "engine/console_engine.hpp"

#include <algorithm>
#include <charconv>
#include <istream>
#include <iterator>
#include <ostream>
#include <stdexcept>

namespace mafia {

namespace {

// Строка ввода без \r (Windows) и крайних пробелов. Пусто — конец ввода.
std::optional<std::string> read_line(std::istream& in) {
    std::string line;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const auto first = line.find_first_not_of(' ');
    const auto last = line.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
}

// Целое — вся строка, без хвостов.
std::optional<int> to_int(std::string_view s) {
    int v = 0;
    const auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (err != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

std::string player_name(const Request& r, PlayerId id) {
    if (r.world && id >= 0 && static_cast<std::size_t>(id) < r.world->players.size()) {
        return r.world->players[static_cast<std::size_t>(id)].name;
    }
    return "Игрок " + std::to_string(id + 1);
}

// «2, 5» — номера игроков для ввода (с 1, как в именах «Игрок N»).
std::string numbered(const std::vector<PlayerId>& ids) {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        out += (i == 0 ? "" : ", ") + std::to_string(ids[i] + 1);
    }
    return out;
}

// Номер игрока из списка допустимых: ввод — «5», ответ — PlayerId 4.
std::optional<PlayerId> pick_player(const std::string& line, const std::vector<PlayerId>& allowed) {
    const std::optional<int> n = to_int(line);
    if (!n) {
        return std::nullopt;
    }
    const PlayerId id = *n - 1;
    return std::ranges::find(allowed, id) != allowed.end() ? std::optional(id) : std::nullopt;
}

// Подсказка ведущего: где мы и кто вы. Личные сведения — только свои.
void header(const Request& r, std::ostream& out) {
    out << "\n>>> Ведущий — вам (" << player_name(r, r.you.self) << ", роль: " << role_name(r.you.role) << ")";
    if (r.world) {
        out << ". День " << r.world->day << ", фаза: " << phase_name(r.world->phase);
    }
    out << ".\n";
    for (const KnownIdentity& k : r.you.known) {
        out << "    Вы знаете: " << player_name(r, k.id) << " — "
            << (k.role ? std::string(role_name(*k.role)) : std::string(k.is_mafia ? "мафия / враг" : "не мафия"))
            << ".\n";
    }
    for (const std::string& n : r.you.notes) {
        out << "    Сведения: " << n << "\n";
    }
}

std::vector<PlayerId> alive_others(const Request& r) {
    std::vector<PlayerId> out;
    if (r.world) {
        for (const PublicPlayer& p : r.world->players) {
            if (p.alive && p.id != r.you.self) {
                out.push_back(p.id);
            }
        }
    }
    return out;
}

void retry(std::ostream& out, const std::string& why) {
    out << "    Не понял: " << why << ". Попробуйте ещё раз.\n> " << std::flush;
}

}  // namespace

ConsoleEngine::ConsoleEngine(SharedPtr<Engine> inner, PlayerId seat, std::istream& in, std::ostream& out)
    : inner_(std::move(inner)), seat_(seat), in_(in), out_(out) {
    name_ = std::string(inner_->name()) + "; пользователь — Игрок " + std::to_string(seat_ + 1);
}

void ConsoleEngine::start(const std::vector<SharedPtr<Player>>& players) {
    if (seat_ < 0 || static_cast<std::size_t>(seat_) >= players.size()) {
        throw std::invalid_argument("ConsoleEngine: места " + std::to_string(seat_ + 1) + " за столом нет");
    }
    inner_->start(players);  // бот на месте пользователя тоже запущен: он нужен после EOF
}

void ConsoleEngine::stop() {
    inner_->stop();
}

std::vector<Response> ConsoleEngine::exchange(std::vector<Request> requests) {
    const auto human = std::ranges::find_if(requests, [this](const Request& r) { return r.you.self == seat_; });
    if (human == requests.end() || input_closed_ || human->kind == RequestKind::Stop) {
        return inner_->exchange(std::move(requests));  // пользователя этот пакет не касается
    }
    const std::size_t at = static_cast<std::size_t>(human - requests.begin());

    // Сначала спрашиваем пользователя, затем остальных — порядок ответов всё
    // равно восстанавливается по позиции запроса.
    std::optional<Response> mine = ask(*human, in_, out_);
    if (!mine) {
        input_closed_ = true;
        out_ << "\n(ввод закончился — дальше за вас решает бот вашей роли)\n" << std::flush;
        return inner_->exchange(std::move(requests));
    }
    Request own = std::move(*human);
    requests.erase(requests.begin() + static_cast<std::ptrdiff_t>(at));
    std::vector<Response> answers = requests.empty() ? std::vector<Response>{} : inner_->exchange(std::move(requests));

    mine->request_id = own.id;
    mine->from = seat_;
    answers.insert(answers.begin() + static_cast<std::ptrdiff_t>(at), std::move(*mine));
    return answers;
}

std::optional<Response> ConsoleEngine::ask(const Request& r, std::istream& in, std::ostream& out) {
    header(r, out);
    Response resp;
    resp.request_id = r.id;
    resp.from = r.you.self;

    switch (r.kind) {
        case RequestKind::Speech: {
            const std::vector<PlayerId> others = alive_others(r);
            out << "Ваша реплика в обсуждении. Можно с утверждением:\n"
                << "  обвинить N [текст]  — «игрок N — мафия»\n"
                << "  защитить N [текст]  — «игрок N — мирный»\n"
                << "Просто текст — без утверждения; пустая строка — промолчать. Игроки в игре: "
                << numbered(others) << "\n> " << std::flush;
            for (;;) {
                const std::optional<std::string> line = read_line(in);
                if (!line) {
                    return std::nullopt;
                }
                const bool accuse = line->starts_with("обвинить") || line->starts_with("Обвинить");
                const bool defend = line->starts_with("защитить") || line->starts_with("Защитить");
                if (!accuse && !defend) {
                    resp.speech = *line;
                    return resp;
                }
                // «обвинить 5 он врёт»: номер — второе слово, остальное — текст.
                const std::string rest = line->substr(line->find(' ') == std::string::npos ? line->size() : line->find(' ') + 1);
                const std::string num = rest.substr(0, rest.find(' '));
                const std::optional<PlayerId> who = pick_player(num, others);
                if (!who) {
                    retry(out, "после «" + std::string(accuse ? "обвинить" : "защитить") +
                                   "» нужен номер живого игрока, кроме вас: " + numbered(others));
                    continue;
                }
                const std::string text = rest.find(' ') == std::string::npos ? "" : rest.substr(rest.find(' ') + 1);
                resp.claim = Claim{*who, accuse};
                resp.speech = text.empty() ? player_name(r, *who) + (accuse ? " — мафия." : " — мирный.") : text;
                return resp;
            }
        }
        case RequestKind::LastWord: {
            out << "Вы выбываете. Последнее слово (пустая строка — промолчать). Прямо называть убийц нельзя.\n> "
                << std::flush;
            const std::optional<std::string> line = read_line(in);
            if (!line) {
                return std::nullopt;
            }
            resp.speech = *line;
            return resp;
        }
        case RequestKind::Vote: {
            out << "Голосование: против кого вы голосуете? Номера: " << numbered(r.targets)
                << (r.can_abstain ? "; 0 — воздержаться" : "") << "\n> " << std::flush;
            for (;;) {
                const std::optional<std::string> line = read_line(in);
                if (!line) {
                    return std::nullopt;
                }
                if (r.can_abstain && *line == "0") {
                    return resp;
                }
                if (const std::optional<PlayerId> who = pick_player(*line, r.targets)) {
                    resp.target = who;
                    resp.speech = "Голосую против: " + player_name(r, *who) + ".";
                    return resp;
                }
                retry(out, "нужен один из номеров " + numbered(r.targets));
            }
        }
        case RequestKind::Choice: {
            switch (r.choice) {
                case ChoiceKind::KickAll:
                    out << "Все кандидаты набрали поровну: " << numbered(r.targets) << ". Выгнать всех разом?\n";
                    break;
                case ChoiceKind::Execute:
                    out << "Вы палач. Ничья между: " << numbered(r.targets) << ". Кого казнить?\n";
                    break;
                case ChoiceKind::Revive:
                    out << "Вы некромант. Раз за игру можно раскрыться и воскресить выбывшего.\n";
                    break;
            }
            for (std::size_t i = 0; i < r.options.size(); ++i) {
                out << "  " << i + 1 << " — " << r.options[i] << "\n";
            }
            out << "> " << std::flush;
            for (;;) {
                const std::optional<std::string> line = read_line(in);
                if (!line) {
                    return std::nullopt;
                }
                const std::optional<int> n = to_int(*line);
                if (n && *n >= 1 && static_cast<std::size_t>(*n) <= r.options.size()) {
                    resp.choice = static_cast<std::size_t>(*n - 1);
                    return resp;
                }
                retry(out, "нужен номер варианта от 1 до " + std::to_string(r.options.size()));
            }
        }
        case RequestKind::NightMove: {
            const bool council = r.actions.size() == 1 && r.actions.front() == NightActionKind::Kill &&
                                 team_of(r.you.role) == Team::Mafia;
            out << (council ? "Ночь. Совет мафии: за кого голосуете? Решение объявит Босс.\n"
                            : "Ночь. Ваш ход — в приват ведущему.\n");
            // 1. Какое действие (если их несколько).
            NightActionKind kind = r.actions.empty() ? NightActionKind::None : r.actions.front();
            if (r.actions.size() > 1) {
                for (std::size_t i = 0; i < r.actions.size(); ++i) {
                    out << "  " << i + 1 << " — " << action_name(r.actions[i]) << "\n";
                }
                if (r.can_abstain) {
                    out << "  0 — пропустить ход\n";
                }
                out << "Действие: > " << std::flush;
                for (;;) {
                    const std::optional<std::string> line = read_line(in);
                    if (!line) {
                        return std::nullopt;
                    }
                    const std::optional<int> n = to_int(*line);
                    if (r.can_abstain && n == 0) {
                        return resp;  // NightActionKind::None — пропуск
                    }
                    if (n && *n >= 1 && static_cast<std::size_t>(*n) <= r.actions.size()) {
                        kind = r.actions[static_cast<std::size_t>(*n - 1)];
                        break;
                    }
                    retry(out, "нужен номер действия от 1 до " + std::to_string(r.actions.size()));
                }
            }
            // 2. Цель.
            out << "Цель (" << action_name(kind) << "): номера " << numbered(r.targets)
                << (r.can_abstain && r.actions.size() == 1 ? "; 0 — пропустить ход" : "") << "\n> " << std::flush;
            for (;;) {
                const std::optional<std::string> line = read_line(in);
                if (!line) {
                    return std::nullopt;
                }
                if (r.can_abstain && r.actions.size() == 1 && *line == "0") {
                    return resp;
                }
                if (const std::optional<PlayerId> who = pick_player(*line, r.targets)) {
                    resp.night.kind = kind;
                    resp.night.target = who;
                    break;
                }
                retry(out, "нужен один из номеров " + numbered(r.targets));
            }
            // 3. Дополнительное: второй игрок журналиста, роль для похитителя.
            if (kind == NightActionKind::Compare) {
                std::vector<PlayerId> rest;
                std::ranges::copy_if(r.targets, std::back_inserter(rest),
                                     [&](PlayerId c) { return c != *resp.night.target; });
                out << "С кем сравнить: номера " << numbered(rest) << "\n> " << std::flush;
                for (;;) {
                    const std::optional<std::string> line = read_line(in);
                    if (!line) {
                        return std::nullopt;
                    }
                    if (const std::optional<PlayerId> who = pick_player(*line, rest)) {
                        resp.night.second = who;
                        break;
                    }
                    retry(out, "нужен один из номеров " + numbered(rest));
                }
            }
            if (kind == NightActionKind::Kidnap) {
                out << "Какая у него роль? (по-русски или латиницей, например «Комиссар» или commissar)\n> "
                    << std::flush;
                for (;;) {
                    const std::optional<std::string> line = read_line(in);
                    if (!line) {
                        return std::nullopt;
                    }
                    if (const std::optional<RoleKind> role = role_by_name(*line)) {
                        resp.night.guess = role;
                        break;
                    }
                    retry(out, "такой роли нет");
                }
            }
            return resp;
        }
        case RequestKind::Stop:
            return resp;
    }
    return resp;
}

}  // namespace mafia
