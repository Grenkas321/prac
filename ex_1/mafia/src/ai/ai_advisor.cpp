#include "ai/ai_advisor.hpp"

#include "ai/json.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <variant>

namespace mafia {

// ---------------------------------------------------------------------------
// Настройки, бюджет, журнал
// ---------------------------------------------------------------------------

std::string AiConfig::key_from_environment() {
    const char* key = std::getenv("MAFIA_AI_KEY");
    return key ? std::string(key) : std::string();
}

bool AiBudget::try_acquire(int round) noexcept {
    if (per_round_ <= 0 || round < 0) {
        return false;
    }
    const std::uint64_t r = static_cast<std::uint32_t>(round);
    std::uint64_t cur = state_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t cur_round = cur >> 32;
        const std::uint64_t used = cur & 0xFFFF'FFFFu;
        std::uint64_t next = 0;
        if (r > cur_round) {
            next = (r << 32) | 1;  // новый круг: счётчик с нуля, и сразу занимаем одно
        } else if (r == cur_round && used < static_cast<std::uint64_t>(per_round_)) {
            next = cur + 1;
        } else {
            return false;  // лимит круга исчерпан (или запоздалый вызов из прошлого круга)
        }
        // Удалось — значит, между чтением и записью никто не вмешался.
        // Не удалось — cur обновлён текущим значением, пробуем снова.
        if (state_.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

int AiBudget::used(int round) const noexcept {
    const std::uint64_t cur = state_.load(std::memory_order_acquire);
    return (cur >> 32) == static_cast<std::uint32_t>(round) ? static_cast<int>(cur & 0xFFFF'FFFFu) : 0;
}

AiDialogLog::AiDialogLog(const std::filesystem::path& file) : path_(file) {
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    out_.open(file, std::ios::out | std::ios::app);
}

void AiDialogLog::write(const std::string& text) {
    std::lock_guard lock(mutex_);
    if (out_) {
        out_ << text << '\n';
        out_.flush();
    }
}

std::string_view personality_for(int index) {
    // Характеристики — по образцу задания.
    static constexpr std::array<std::string_view, 6> kTraits = {
        "осторожный аналитик: говоришь мало, опираешься на голоса и раскрытые роли",
        "плохо умеешь врать: если ты мафия, иногда выдаёшь себя волнением",
        "никому не доверяешь и прямо говоришь об этом",
        "эмоциональный: быстро обвиняешь и громко защищаешься",
        "играя за мирного, нарочно ведёшь себя как мафия, чтобы проверить реакцию стола",
        "всегда поддерживаешь игрока, который первым заговорил в этот день",
    };
    return kTraits[static_cast<std::size_t>(index) % kTraits.size()];
}

// ---------------------------------------------------------------------------
// Запрос к модели
// ---------------------------------------------------------------------------

namespace {

std::string player_name(const Request& r, PlayerId id) {
    if (r.world && id >= 0 && static_cast<std::size_t>(id) < r.world->players.size()) {
        return r.world->players[static_cast<std::size_t>(id)].name;
    }
    return "Игрок " + std::to_string(id + 1);
}

std::string numbers(const std::vector<PlayerId>& ids) {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        out += (i == 0 ? "" : ", ") + std::to_string(ids[i] + 1);
    }
    return out;
}

std::string task_code(RequestKind kind) {
    switch (kind) {
        case RequestKind::Speech:
            return "speech";
        case RequestKind::Vote:
            return "vote";
        case RequestKind::LastWord:
            return "last_word";
        default:
            return "none";
    }
}

std::string describe(const Request& r, const HistoryEntry& h) {
    const std::string when = "день " + std::to_string(h.day) + ", " + std::string(phase_name(h.phase)) + ": ";
    if (const auto* s = std::get_if<SpeechEvent>(&h.event)) {
        std::string line = when + player_name(r, s->speaker) + ": «" + s->text + "»";
        if (s->claim) {
            line += s->claim->mafia ? " [обвиняет " : " [защищает ";
            line += player_name(r, s->claim->about) + "]";
        }
        return line;
    }
    if (const auto* v = std::get_if<VoteEvent>(&h.event)) {
        return when + player_name(r, v->voter) + " голосует против " +
               (v->target ? player_name(r, *v->target) : std::string("— (не засчитан)"));
    }
    if (const auto* d = std::get_if<DeathEvent>(&h.event)) {
        std::string line = when + "выбыл " + player_name(r, d->victim) +
                           (d->disclosed.role ? " (" + std::string(role_name(*d->disclosed.role)) + ")"
                                              : (d->disclosed.is_mafia ? " (мафия)" : " (не мафия)"));
        if (d->killed_by) {
            line += ", убит: " + std::string(role_name(*d->killed_by));
        }
        return line;
    }
    if (const auto* w = std::get_if<LastWordEvent>(&h.event)) {
        return when + "последнее слово " + player_name(r, w->speaker) + ": «" + w->text + "»";
    }
    if (const auto* a = std::get_if<AnnouncementEvent>(&h.event)) {
        return when + "ведущий: " + a->text;
    }
    if (const auto* p = std::get_if<PollEvent>(&h.event)) {
        return when + player_name(r, p->voter) + (p->agree ? " — за" : " — против") + " того, чтобы выгнать всех";
    }
    if (const auto* sv = std::get_if<SavedEvent>(&h.event)) {
        return when + "на " + player_name(r, sv->target) + " напали ночью, но спасли";
    }
    if (const auto* rv = std::get_if<RevealEvent>(&h.event)) {
        return when + "раскрыт " + player_name(r, rv->target) +
               (rv->disclosed.role ? ": " + std::string(role_name(*rv->disclosed.role)) : std::string());
    }
    if (const auto* rv = std::get_if<ReviveEvent>(&h.event)) {
        return when + "вернулся в игру " + player_name(r, rv->target);
    }
    return when + "событие";
}

}  // namespace

AiPrompt build_prompt(const Request& r, std::string_view personality) {
    const PlayerId self = r.you.self;
    AiPrompt p;

    // --- Системная часть: кто ты, правила, характер, твои тайны, формат ---
    p.system = "Ты — участник игры «Мафия», " + player_name(r, self) +
               ". Ведущий — программа, ты отвечаешь ему только JSON.\n"
               "Твоя роль: " + std::string(role_name(r.you.role)) + " (команда: " +
               std::string(team_name(team_of(r.you.role))) + "). " + std::string(role_description(r.you.role)) +
               "\nТвой характер: " + std::string(personality) +
               ".\nПравила: днём все обсуждают и голосуют против подозреваемого; ночью мафия и роли со "
               "способностями действуют тайно. Мирные побеждают, когда выбыли мафия и одиночки-убийцы; "
               "мафия — когда её не меньше, чем остальных.\n";
    std::string secrets;
    for (const KnownIdentity& k : r.you.known) {
        secrets += "- " + player_name(r, k.id) + ": " + (k.is_mafia ? "мафия/враг" : "не мафия") +
                   (k.role ? " (" + std::string(role_name(*k.role)) + ")" : std::string()) + "\n";
    }
    for (const std::string& n : r.you.notes) {
        secrets += "- " + n + "\n";
    }
    p.system += "Что знаешь только ты (вслух раскрывай, только если это тебе выгодно):\n" +
                (secrets.empty() ? std::string("- ничего сверх публичного\n") : secrets);
    p.system +=
        "Ответ — ТОЛЬКО один JSON-объект, без текста вокруг:\n"
        "{\"action\": \"<действие>\", \"target\": <номер игрока или null>, "
        "\"reasoning\": \"<почему так; видит только ведущий>\", \"message\": \"<что говоришь вслух, до 300 символов>\"}\n";

    // --- Сообщение: публичное состояние и задание ---
    std::string& u = p.user;
    u = "День " + std::to_string(r.world ? r.world->day : 0) + ". Ты — " + player_name(r, self) + ".\nЗа столом:";
    if (r.world) {
        for (const PublicPlayer& pp : r.world->players) {
            u += "\n- " + pp.name + (pp.alive ? " — в игре" : " — выбыл");
            if (pp.disclosed) {
                u += pp.disclosed->role ? " (" + std::string(role_name(*pp.disclosed->role)) + ")"
                                        : (pp.disclosed->is_mafia ? " (мафия)" : " (не мафия)");
            }
        }
        u += "\nПоследние публичные события:";
        const auto& h = r.world->history;
        const std::size_t from = h.size() > 60 ? h.size() - 60 : 0;  // хватит контекста и не раздуем запрос
        for (std::size_t i = from; i < h.size(); ++i) {
            u += "\n- " + describe(r, h[i]);
        }
    }
    u += "\n\nЗАДАНИЕ: " + task_code(r.kind) + "\n";
    switch (r.kind) {
        case RequestKind::Speech:
            u += "Твоя речь в обсуждении. action: \"accuse\" (обвинить target), \"defend\" (защитить target) "
                 "или \"say\" (просто сказать, target = null).\n";
            break;
        case RequestKind::Vote:
            u += "Голосование: action = \"vote\", target — против кого голосуешь (только из допустимых). "
                 "message — короткое пояснение вслух.\n";
            break;
        case RequestKind::LastWord:
            u += "Ты выбываешь. Последнее слово: action = \"say\", target = null. "
                 "Нельзя прямо указывать на своих убийц.\n";
            break;
        default:
            break;
    }
    if (r.kind == RequestKind::Vote) {
        u += "Допустимые цели (номера): " + numbers(r.targets) + "\n";
    } else if (r.kind == RequestKind::Speech && r.world) {
        std::vector<PlayerId> others;
        for (const PublicPlayer& pp : r.world->players) {
            if (pp.alive && pp.id != self) {
                others.push_back(pp.id);
            }
        }
        u += "Допустимые цели (номера): " + numbers(others) + "\n";
    }
    return p;
}

std::string request_body(const AiConfig& c, const AiPrompt& p) {
    const std::string tokens = std::to_string(c.max_tokens);
    if (c.format == AiFormat::Anthropic) {
        return "{\"model\":" + json::quote(c.model) + ",\"max_tokens\":" + tokens +
               ",\"system\":" + json::quote(p.system) +
               ",\"messages\":[{\"role\":\"user\",\"content\":" + json::quote(p.user) + "}]}";
    }
    return "{\"model\":" + json::quote(c.model) + ",\"max_tokens\":" + tokens + ",\"temperature\":0.7" +
           ",\"messages\":[{\"role\":\"system\",\"content\":" + json::quote(p.system) +
           "},{\"role\":\"user\",\"content\":" + json::quote(p.user) + "}]}";
}

// ---------------------------------------------------------------------------
// Разбор и проверка ответа
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxMessage = 300;  // символов (не байтов)

// Убрать управляющие символы, обрезать по числу символов UTF-8.
std::string clean_text(const std::string& s) {
    std::string out;
    std::size_t symbols = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x20) {
            out += ' ';  // перевод строки и прочее — в пробел: реплика одна строкой
            ++symbols;
            continue;
        }
        if ((c & 0xC0) != 0x80) {  // начало символа
            if (symbols == kMaxMessage) {
                out += "…";
                break;
            }
            ++symbols;
        }
        out += static_cast<char>(c);
    }
    const auto first = out.find_first_not_of(' ');
    const auto last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
}

// Цель: число 5, строка "5" или "Игрок 5" -> PlayerId 4.
std::optional<PlayerId> parse_target(const json::Value& v) {
    if (const double* d = v.as_number()) {
        if (*d >= 1 && *d <= 1000 && *d == static_cast<double>(static_cast<int>(*d))) {
            return static_cast<PlayerId>(*d) - 1;
        }
        return std::nullopt;
    }
    if (const std::string* s = v.as_string()) {
        const auto digits = s->find_first_of("0123456789");
        if (digits == std::string::npos) {
            return std::nullopt;
        }
        int n = 0;
        for (std::size_t i = digits; i < s->size() && (*s)[i] >= '0' && (*s)[i] <= '9' && n < 10000; ++i) {
            n = n * 10 + ((*s)[i] - '0');
        }
        return n >= 1 ? std::optional<PlayerId>(n - 1) : std::nullopt;
    }
    return std::nullopt;
}

// Текст ответа модели из ответа сервиса.
std::optional<std::string> model_text(const json::Value& root, AiFormat format, std::string& error) {
    if (const json::Value* err = root.get("error")) {
        const json::Value* msg = err->get("message");
        error = "сервис вернул ошибку: " + (msg && msg->as_string() ? *msg->as_string() : std::string("без описания"));
        return std::nullopt;
    }
    const json::Value* text = nullptr;
    if (format == AiFormat::OpenAI) {
        const json::Value* choices = root.get("choices");
        const json::Array* arr = choices ? choices->as_array() : nullptr;
        if (arr && !arr->empty()) {
            const json::Value* message = (*arr)[0].get("message");
            text = message ? message->get("content") : nullptr;
        }
    } else {
        const json::Value* content = root.get("content");
        const json::Array* arr = content ? content->as_array() : nullptr;
        if (arr) {
            for (const json::Value& block : *arr) {
                const json::Value* type = block.get("type");
                if (type && type->as_string() && *type->as_string() == "text") {
                    text = block.get("text");
                    break;
                }
            }
        }
    }
    if (!text || !text->as_string()) {
        error = "в ответе сервиса нет текста модели";
        return std::nullopt;
    }
    return *text->as_string();
}

}  // namespace

ParsedReply parse_reply(const Request& r, AiFormat format, std::string_view body) {
    ParsedReply out;
    // 1. Ответ сервиса — JSON.
    json::Value root;
    try {
        root = json::parse(body);
    } catch (const json::ParseError& e) {
        out.error = std::string("ответ сервиса — не JSON (") + e.what() + ")";
        return out;
    }
    // 2. В нём есть текст модели.
    const std::optional<std::string> text = model_text(root, format, out.error);
    if (!text) {
        return out;
    }
    // 3. Текст модели — JSON-объект. Модели любят обрамлять его ```json ... ```
    //    или фразой — берём от первой «{» до последней «}».
    const std::size_t open = text->find('{');
    const std::size_t close = text->rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        out.error = "модель ответила не JSON-объектом";
        return out;
    }
    json::Value reply;
    try {
        reply = json::parse(std::string_view(*text).substr(open, close - open + 1));
    } catch (const json::ParseError& e) {
        out.error = std::string("JSON модели не разобрался (") + e.what() + ")";
        return out;
    }
    // 4. Обязательные поля и их типы.
    const json::Value* action = reply.get("action");
    const json::Value* message = reply.get("message");
    const json::Value* reasoning = reply.get("reasoning");
    const json::Value* target = reply.get("target");
    if (!action || !action->as_string() || !message || !message->as_string() || !reasoning ||
        !reasoning->as_string() || !target) {
        out.error = "нет обязательных полей action, target, reasoning, message (или не тех типов)";
        return out;
    }
    out.reasoning = clean_text(*reasoning->as_string());
    const std::string& act = *action->as_string();
    const std::string said = clean_text(*message->as_string());

    // 5. Действие подходит к запросу; цель — из допустимых.
    AdvisorAnswer a;
    a.speech = said;
    const std::optional<PlayerId> who = target->is_null() ? std::nullopt : parse_target(*target);
    switch (r.kind) {
        case RequestKind::Vote: {
            if (act != "vote") {
                out.error = "на голосование ответ должен быть action = vote, а не «" + act + "»";
                return out;
            }
            if (!who || std::ranges::find(r.targets, *who) == r.targets.end()) {
                out.error = "недопустимая цель голоса";
                return out;
            }
            a.target = who;
            if (a.speech.empty()) {
                a.speech = "Голосую против: " + player_name(r, *who) + ".";
            }
            break;
        }
        case RequestKind::Speech: {
            if (act == "accuse" || act == "defend") {
                const bool ok = who && r.world && *who != r.you.self && *who >= 0 &&
                                static_cast<std::size_t>(*who) < r.world->players.size() &&
                                r.world->players[static_cast<std::size_t>(*who)].alive;
                if (!ok) {
                    out.error = "недопустимая цель обвинения/защиты";
                    return out;
                }
                a.target = who;
                a.claim = Claim{*who, act == "accuse"};
            } else if (act != "say") {
                out.error = "в обсуждении допустимы accuse, defend, say, а не «" + act + "»";
                return out;
            }
            if (a.speech.empty()) {
                out.error = "пустая реплика";
                return out;
            }
            break;
        }
        case RequestKind::LastWord:
            if (act != "say" || a.speech.empty()) {
                out.error = "последнее слово — action = say и непустая реплика";
                return out;
            }
            break;
        default:
            out.error = "на такие запросы ИИ не отвечает";
            return out;
    }
    out.answer = std::move(a);
    return out;
}

// ---------------------------------------------------------------------------
// Советник
// ---------------------------------------------------------------------------

AiAdvisor::AiAdvisor(SharedPtr<const AiConfig> config, SharedPtr<HttpClient> client, SharedPtr<AiBudget> budget,
                     SharedPtr<AiDialogLog> log, std::string personality)
    : config_(std::move(config)),
      client_(std::move(client)),
      budget_(std::move(budget)),
      log_(std::move(log)),
      personality_(std::move(personality)) {}

std::optional<AdvisorAnswer> AiAdvisor::advise(const Request& request) {
    if (!request.world ||
        (request.kind != RequestKind::Speech && request.kind != RequestKind::Vote && request.kind != RequestKind::LastWord)) {
        return std::nullopt;
    }
    const std::string who = player_name(request, request.you.self);
    const std::string head = "=== " + who + " (" + std::string(role_name(request.you.role)) + "), день " +
                             std::to_string(request.world->day) + ", запрос #" + std::to_string(request.id) + ": " +
                             task_code(request.kind) + " ===";
    auto log = [&](const std::string& text) {
        if (log_) {
            log_->write(text);
        }
    };
    if (!budget_->try_acquire(request.world->day)) {
        log(head + "\nлимит обращений за круг исчерпан (" + std::to_string(budget_->per_round()) + ") — эвристика\n");
        return std::nullopt;
    }

    const AiPrompt prompt = build_prompt(request, personality_);
    const std::string body = request_body(*config_, prompt);
    std::vector<std::string> headers = {"Content-Type: application/json"};
    std::string path = "/chat/completions";
    if (config_->format == AiFormat::Anthropic) {
        path = "/messages";
        headers.push_back("anthropic-version: 2023-06-01");
        if (!config_->api_key.empty()) {
            headers.push_back("x-api-key: " + config_->api_key);
        }
    } else if (!config_->api_key.empty()) {
        headers.push_back("Authorization: Bearer " + config_->api_key);
    }
    // В журнал — запрос без заголовков: ключ живёт только в них.
    log(head + "\n[системная часть]\n" + prompt.system + "[сообщение]\n" + prompt.user);

    const HttpResult result = client_->post(config_->base_url + path, headers, body, config_->timeout);
    if (!result.error.empty()) {
        log("[сбой] " + result.error + " — эвристика\n");
        return std::nullopt;
    }
    log("[ответ HTTP " + std::to_string(result.status) + "]\n" + result.body.substr(0, 4000));
    if (result.status != 200) {
        log("[отвергнут] код ответа " + std::to_string(result.status) + " — эвристика\n");
        return std::nullopt;
    }
    ParsedReply parsed = parse_reply(request, config_->format, result.body);
    if (!parsed.answer) {
        log("[отвергнут] " + parsed.error + " — эвристика\n");
        return std::nullopt;
    }
    log("[принят] обоснование: " + parsed.reasoning + "\n");
    return parsed.answer;
}

}  // namespace mafia
