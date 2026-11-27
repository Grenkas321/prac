// Тесты ИИ-агента (этап 16). Настоящий сервис не нужен: все обращения идут
// в локальный поддельный сервер (fake_llm_server.hpp) через настоящий curl.
//
//   * JSON-разбор: корректный ввод, ошибки, глубина;
//   * запрос к модели: системная часть и сообщение, чужих тайн нет;
//   * разбор ответа: успех каждого вида, все виды отказа;
//   * транспорт: коды 500/429/401, таймаут, отказ в соединении, обрыв, мусор;
//   * ключ: уходит только в заголовке, не попадает в журнал и в вывод;
//   * лимит за круг: исчерпание, сброс, атомарность под нагрузкой из нитей;
//   * игрок: любой сбой советника — эвристика;
//   * целые партии с ИИ-игроками на обоих движках.
#include "ai/ai_advisor.hpp"
#include "ai/http_client.hpp"
#include "ai/json.hpp"
#include "engine/coro_engine.hpp"
#include "engine/thread_engine.hpp"
#include "fake_llm_server.hpp"
#include "game/host.hpp"
#include "roles/player_factory.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace mafia;

namespace {

constexpr const char* kKey = "sk-test-DO-NOT-LEAK-42";

// --- Мир для запросов: 6 игроков, второй день ---------------------------------

SharedPtr<const PublicSnapshot> world(int day = 2) {
    auto w = make_shared_ptr<PublicSnapshot>();
    w->day = day;
    w->phase = Phase::Discussion;
    w->mafia_total = 2;
    for (PlayerId i = 0; i < 6; ++i) {
        PublicPlayer p;
        p.id = i;
        p.name = "Игрок " + std::to_string(i + 1);
        w->players.push_back(p);
    }
    w->players[5].alive = false;
    w->players[5].disclosed = Disclosure{false, std::nullopt};
    w->history.push_back({1, Phase::Discussion, SpeechEvent{2, "Игрок 2 какой-то тихий.", Claim{1, true}}});
    w->history.push_back({2, Phase::Morning, AnnouncementEvent{"Ночью выбыл Игрок 6."}});
    return w;
}

// Мафиози (Игрок 1), союзник — Игрок 2. Это его тайна.
Request request(RequestKind kind, int day = 2) {
    Request r;
    r.id = 7;
    r.kind = kind;
    r.world = world(day);
    r.you.self = 0;
    r.you.role = RoleKind::Mafioso;
    r.you.known.push_back(KnownIdentity{1, true, RoleKind::Mafioso});
    if (kind == RequestKind::Vote) {
        r.targets = {1, 2, 3, 4};
    }
    return r;
}

std::string openai_body(const std::string& text) {
    return "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":" + json::quote(text) + "}}]}";
}
std::string anthropic_body(const std::string& text) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json::quote(text) + "}]}";
}

ParsedReply parse(RequestKind kind, const std::string& model_text) {
    return parse_reply(request(kind), AiFormat::OpenAI, openai_body(model_text));
}

std::filesystem::path temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("mafia_test_ai_" + std::to_string(::getpid())) / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Советник, направленный на поддельный сервер.
struct Rig {
    fake::Server server;
    SharedPtr<AiDialogLog> log;
    SharedPtr<AiBudget> budget;
    SharedPtr<AiAdvisor> advisor;

    explicit Rig(const std::string& name, AiFormat format = AiFormat::OpenAI, std::string key = kKey, int limit = 100,
                 int timeout_ms = 5000) {
        auto c = make_shared_ptr<AiConfig>();
        c->base_url = server.url();
        c->format = format;
        c->api_key = std::move(key);
        c->calls_per_round = limit;
        c->timeout = std::chrono::milliseconds(timeout_ms);
        log = make_shared_ptr<AiDialogLog>(temp_dir(name) / "ai_dialog.txt");
        budget = make_shared_ptr<AiBudget>(limit);
        advisor = make_shared_ptr<AiAdvisor>(SharedPtr<const AiConfig>(c), make_shared_ptr<CurlClient>(), budget, log,
                                             std::string(personality_for(0)));
    }
    std::optional<AdvisorAnswer> ask(RequestKind kind, int day = 2) { return advisor->advise(request(kind, day)); }
    std::string dialog() const { return read_file(log->path()); }
};

// ============================================================================
// JSON
// ============================================================================

void json_parses_valid_documents() {
    const json::Value v = json::parse(R"( {"a": [1, -2.5e1, true, false, null], "s": "x\"\\\n\u0416\ud83d\ude00"} )");
    const json::Array* a = v.get("a")->as_array();
    CHECK(a && a->size() == 5);
    CHECK(*(*a)[1].as_number() == -25.0);
    CHECK(*(*a)[2].as_bool() == true);
    CHECK((*a)[4].is_null());
    CHECK(*v.get("s")->as_string() == "x\"\\\nЖ\xF0\x9F\x98\x80");  // \u0416 = Ж, пара суррогатов = 😀
    CHECK(v.get("нет") == nullptr);
    // quote -> parse — та же строка.
    const std::string tricky = "кавычка \" слэш \\ перевод\nтаб\t\x01";
    CHECK(*json::parse(json::quote(tricky)).as_string() == tricky);
}

void json_rejects_malformed_documents() {
    int rejected = 0;
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"не закрыта", "01", "1 2",
                            "\"\\uD800\"", "{\"a\":1,}", "\"\x01\"", "nul"}) {
        try {
            json::parse(bad);
        } catch (const json::ParseError&) {
            ++rejected;
        }
    }
    CHECK(rejected == 13);
    // Глубокая вложенность — ошибка разбора, а не переполнение стека.
    bool deep = false;
    try {
        json::parse(std::string(100000, '['));
    } catch (const json::ParseError&) {
        deep = true;
    }
    CHECK(deep);
}

// ============================================================================
// Запрос к модели
// ============================================================================

void prompt_splits_system_and_message() {
    const AiPrompt p = build_prompt(request(RequestKind::Vote), "осторожный");
    // Системная часть: роль, характер, своя тайна, формат ответа.
    CHECK(p.system.find("Мафиози") != std::string::npos);
    CHECK(p.system.find("осторожный") != std::string::npos);
    CHECK(p.system.find("Игрок 2: мафия/враг") != std::string::npos);
    CHECK(p.system.find("\"action\"") != std::string::npos);
    // Сообщение: публичное и задание с допустимыми целями.
    CHECK(p.user.find("ЗАДАНИЕ: vote") != std::string::npos);
    CHECK(p.user.find("Допустимые цели (номера): 2, 3, 4, 5") != std::string::npos);
    CHECK(p.user.find("Игрок 6 — выбыл (не мафия)") != std::string::npos);
    CHECK(p.user.find("Игрок 2 какой-то тихий") != std::string::npos);
    // Тайна — только в системной части, в сообщение не утекает.
    CHECK(p.user.find("мафия/враг") == std::string::npos);
    CHECK(p.user.find("Мафиози") == std::string::npos);

    // Тело запроса — корректный JSON обоих форматов.
    AiConfig c;
    const json::Value o = json::parse(request_body(c, p));
    CHECK(o.get("messages")->as_array()->size() == 2);
    CHECK(*(*o.get("messages")->as_array())[0].get("role")->as_string() == "system");
    c.format = AiFormat::Anthropic;
    const json::Value a = json::parse(request_body(c, p));
    CHECK(*a.get("system")->as_string() == p.system);
    CHECK(a.get("messages")->as_array()->size() == 1);
}

void prompt_has_no_secrets_of_others() {
    // Мирный ничего тайного не знает — и в запросе ничего тайного нет.
    Request r = request(RequestKind::Speech);
    r.you.role = RoleKind::Civilian;
    r.you.known.clear();
    const AiPrompt p = build_prompt(r, "x");
    CHECK(p.system.find("ничего сверх публичного") != std::string::npos);
    CHECK((p.system + p.user).find("мафия/враг") == std::string::npos);
    // Ключ в запрос не входит вовсе: build_prompt его не получает.
    CHECK((p.system + p.user).find("sk-") == std::string::npos);
}

// ============================================================================
// Разбор ответа (без сети)
// ============================================================================

void reply_success_for_every_kind() {
    ParsedReply v = parse(RequestKind::Vote,
                          R"({"action":"vote","target":3,"reasoning":"молчит","message":"Против третьего."})");
    CHECK(v.answer && v.answer->target == 2 && v.answer->speech == "Против третьего.");
    CHECK(v.reasoning == "молчит");

    ParsedReply s = parse(RequestKind::Speech,
                          R"({"action":"defend","target":"Игрок 4","reasoning":"r","message":"Четвёртый честный."})");
    CHECK(s.answer && s.answer->claim && s.answer->claim->about == 3 && !s.answer->claim->mafia);

    ParsedReply say = parse(RequestKind::Speech, R"({"action":"say","target":null,"reasoning":"r","message":"Думаю."})");
    CHECK(say.answer && !say.answer->claim);

    ParsedReply lw = parse(RequestKind::LastWord, R"({"action":"say","target":null,"reasoning":"r","message":"Пока!"})");
    CHECK(lw.answer && lw.answer->speech == "Пока!");

    // Формат Anthropic.
    ParsedReply an = parse_reply(request(RequestKind::Vote), AiFormat::Anthropic,
                                 anthropic_body(R"({"action":"vote","target":2,"reasoning":"r","message":"м"})"));
    CHECK(an.answer && an.answer->target == 1);
}

void reply_tolerates_wrapping_and_cleans_text() {
    // Обрамление ```json ... ``` и текст вокруг.
    ParsedReply f = parse(RequestKind::Vote,
                          "Конечно! ```json\n{\"action\":\"vote\",\"target\":4,\"reasoning\":\"r\",\"message\":\"x\"}\n```");
    CHECK(f.answer && f.answer->target == 3);
    // Переводы строк и управляющие символы — в пробелы; длина ограничена.
    ParsedReply c = parse(RequestKind::Speech, "{\"action\":\"say\",\"target\":null,\"reasoning\":\"r\",\"message\":\"а\\nб\\u0007в\"}");
    CHECK(c.answer && c.answer->speech == "а б в");
    ParsedReply l = parse(RequestKind::Speech, "{\"action\":\"say\",\"target\":null,\"reasoning\":\"r\",\"message\":\"" +
                                                   std::string(1000, 'z') + "\"}");
    CHECK(l.answer && l.answer->speech.size() < 320);
    // Пустая реплика на голос — подставляется своя.
    ParsedReply e = parse(RequestKind::Vote, R"({"action":"vote","target":2,"reasoning":"r","message":""})");
    CHECK(e.answer && !e.answer->speech.empty());
}

void reply_rejections() {
    struct Case {
        RequestKind kind;
        std::string text;
        const char* why;
    };
    const std::vector<Case> cases = {
        {RequestKind::Vote, R"({"action":"vote","target":6,"reasoning":"r","message":"m"})", "мёртвый — не из допустимых"},
        {RequestKind::Vote, R"({"action":"vote","target":1,"reasoning":"r","message":"m"})", "сам за себя"},
        {RequestKind::Vote, R"({"action":"vote","target":99,"reasoning":"r","message":"m"})", "нет такого игрока"},
        {RequestKind::Vote, R"({"action":"vote","target":2.5,"reasoning":"r","message":"m"})", "дробная цель"},
        {RequestKind::Vote, R"({"action":"vote","target":null,"reasoning":"r","message":"m"})", "голос без цели"},
        {RequestKind::Vote, R"({"action":"kill","target":2,"reasoning":"r","message":"m"})", "чужое действие"},
        {RequestKind::Speech, R"({"action":"vote","target":2,"reasoning":"r","message":"m"})", "голос в речи"},
        {RequestKind::Speech, R"({"action":"accuse","target":1,"reasoning":"r","message":"m"})", "обвиняет себя"},
        {RequestKind::Speech, R"({"action":"accuse","target":6,"reasoning":"r","message":"m"})", "обвиняет выбывшего"},
        {RequestKind::Speech, R"({"action":"accuse","target":"никого","reasoning":"r","message":"m"})", "цель-не число"},
        {RequestKind::Speech, R"({"action":"say","target":null,"reasoning":"r","message":"   "})", "пустая речь"},
        {RequestKind::LastWord, R"({"action":"accuse","target":2,"reasoning":"r","message":"m"})", "обвинение в посл. слове"},
        {RequestKind::Vote, R"({"target":2,"reasoning":"r","message":"m"})", "нет action"},
        {RequestKind::Vote, R"({"action":"vote","reasoning":"r","message":"m"})", "нет target"},
        {RequestKind::Vote, R"({"action":"vote","target":2,"message":"m"})", "нет reasoning"},
        {RequestKind::Vote, R"({"action":"vote","target":2,"reasoning":"r"})", "нет message"},
        {RequestKind::Vote, R"({"action":1,"target":2,"reasoning":"r","message":"m"})", "action — число"},
        {RequestKind::Vote, R"({"action":"vote","target":2,"reasoning":"r","message":["m"]})", "message — массив"},
        {RequestKind::Vote, "Я голосую против Игрока 3.", "не JSON"},
        {RequestKind::Vote, R"({"action":"vote","target":2,)", "оборванный JSON"},
        {RequestKind::Vote, "[1,2,3]", "массив вместо объекта"},
    };
    for (const Case& c : cases) {
        const ParsedReply r = parse(c.kind, c.text);
        if (r.answer || r.error.empty()) {
            std::cout << "\n      не отвергнут: " << c.why;
        }
        CHECK(!r.answer && !r.error.empty());
    }
    // Ошибки уровня сервиса.
    const Request v = request(RequestKind::Vote);
    for (const char* body : {"<html>502 Bad Gateway</html>", "{}", R"({"choices":[]})",
                             R"({"choices":[{"message":{"content":null}}]})",
                             R"({"error":{"message":"rate limit","type":"requests"}})"}) {
        CHECK(!parse_reply(v, AiFormat::OpenAI, body).answer);
    }
    CHECK(!parse_reply(v, AiFormat::Anthropic, R"({"content":[{"type":"tool_use","id":"x"}]})").answer);
    // Ответ одного формата при ожидании другого — тоже отказ.
    CHECK(!parse_reply(v, AiFormat::Anthropic, openai_body(R"({"action":"vote","target":2,"reasoning":"r","message":"m"})")).answer);
    // Ночные ходы и выборы ИИ не делает.
    CHECK(!parse(RequestKind::NightMove, R"({"action":"say","target":null,"reasoning":"r","message":"m"})").answer);
}

// ============================================================================
// Через поддельный сервер: успех и ключ
// ============================================================================

void server_success_openai_and_key_handling() {
    Rig rig("openai");
    const auto speech = rig.ask(RequestKind::Speech);
    const auto vote = rig.ask(RequestKind::Vote);
    const auto last = rig.ask(RequestKind::LastWord);
    CHECK(speech && speech->claim && speech->claim->mafia && speech->claim->about == 1);
    CHECK(vote && vote->target == 1);
    CHECK(last && last->speech == "Удачи городу!");

    const std::vector<fake::Received> got = rig.server.received();
    CHECK(got.size() == 3);
    for (const fake::Received& r : got) {
        CHECK(r.path == "/v1/chat/completions");
        CHECK(r.headers.find(std::string("Authorization: Bearer ") + kKey) != std::string::npos);
        CHECK(r.headers.find("application/json") != std::string::npos);
        CHECK(r.body.find(kKey) == std::string::npos);  // ключ — только в заголовке
    }
    const std::string dialog = rig.dialog();
    CHECK(dialog.find("[системная часть]") != std::string::npos);
    CHECK(dialog.find("[принят]") != std::string::npos);
    CHECK(dialog.find(kKey) == std::string::npos);  // и не в журнале
    CHECK(dialog.find("DO-NOT-LEAK") == std::string::npos);
}

void server_success_anthropic() {
    Rig rig("anthropic", AiFormat::Anthropic);
    CHECK(rig.ask(RequestKind::Vote));
    const auto got = rig.server.received();
    CHECK(got.size() == 1 && got[0].path == "/v1/messages");
    CHECK(got[0].headers.find(std::string("x-api-key: ") + kKey) != std::string::npos);
    CHECK(got[0].headers.find("anthropic-version: 2023-06-01") != std::string::npos);
    CHECK(got[0].headers.find("Authorization") == std::string::npos);
}

void no_key_means_no_auth_header() {
    Rig rig("nokey", AiFormat::OpenAI, "");
    CHECK(rig.ask(RequestKind::Vote));  // Vireonix-подобный сервис — ключ не нужен
    CHECK(rig.server.received()[0].headers.find("Authorization") == std::string::npos);
}

// ============================================================================
// Через поддельный сервер: все виды сбоев -> «совета нет» и запись в журнале
// ============================================================================

void server_http_errors() {
    Rig rig("http");
    for (int code : {500, 429, 401, 404, 503}) {
        rig.server.push(fake::Script::reply(code, R"({"error":{"message":"плохо"}})"));
        CHECK(!rig.ask(RequestKind::Vote));
    }
    // 200, но не тот формат / ошибка в теле / недопустимый ответ модели.
    rig.server.push(fake::Script::reply(200, "<html>не JSON</html>"));
    rig.server.push(fake::Script::reply(200, R"({"error":{"message":"rate limit"}})"));
    rig.server.push(fake::Script::content("Я думаю, мафия — Игрок 3."));
    rig.server.push(fake::Script::content(R"({"action":"vote","target":42,"reasoning":"r","message":"m"})"));
    rig.server.push(fake::Script::content(R"({"action":"vote","target":2})"));
    for (int i = 0; i < 5; ++i) {
        CHECK(!rig.ask(RequestKind::Vote));
    }
    // После всех сбоев — снова работает.
    CHECK(rig.ask(RequestKind::Vote));
    const std::string d = rig.dialog();
    CHECK(d.find("код ответа 500") != std::string::npos);
    CHECK(d.find("код ответа 429") != std::string::npos);
    CHECK(d.find("код ответа 401") != std::string::npos);
    CHECK(d.find("не JSON") != std::string::npos);
    CHECK(d.find("rate limit") != std::string::npos);
    CHECK(d.find("недопустимая цель голоса") != std::string::npos);
    CHECK(d.find("нет обязательных полей") != std::string::npos);
}

void server_transport_failures() {
    Rig rig("transport", AiFormat::OpenAI, kKey, 100, /*timeout_ms=*/1000);
    // Таймаут: сервер молчит дольше, чем мы ждём.
    rig.server.push(fake::Script::smart().after(2500));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!rig.ask(RequestKind::Vote));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2400));
    // Обрыв без ответа и не-HTTP вместо ответа.
    rig.server.push(fake::Script::close());
    CHECK(!rig.ask(RequestKind::Vote));
    rig.server.push(fake::Script::garbage());
    CHECK(!rig.ask(RequestKind::Vote));
    const std::string d = rig.dialog();
    CHECK(d.find("превышено время ожидания") != std::string::npos);
    CHECK(d.find("[сбой] curl завершился с кодом") != std::string::npos);

    // Отказ в соединении: порт, на котором уже никто не слушает.
    std::string dead;
    {
        fake::Server gone;
        dead = gone.url();
    }
    CurlClient curl;
    const HttpResult refused = curl.post(dead + "/chat/completions", {}, "{}", std::chrono::milliseconds(2000));
    CHECK(refused.error == "не удалось подключиться");
    // Чужая схема адреса — curl даже не запускается.
    CHECK(!curl.post("file:///etc/passwd", {}, "{}", std::chrono::milliseconds(1000)).error.empty());
}

// ============================================================================
// Лимит обращений за круг
// ============================================================================

void budget_limits_calls_per_round() {
    Rig rig("budget", AiFormat::OpenAI, kKey, /*limit=*/2);
    CHECK(rig.ask(RequestKind::Vote, 3));
    CHECK(rig.ask(RequestKind::Vote, 3));
    CHECK(!rig.ask(RequestKind::Vote, 3));  // третье — за лимитом
    CHECK(rig.server.count() == 2);         // и в сеть не уходило
    CHECK(rig.budget->used(3) == 2);
    CHECK(rig.ask(RequestKind::Vote, 4));   // новый круг — счётчик сброшен
    CHECK(rig.budget->used(4) == 1 && rig.budget->used(3) == 0);
    CHECK(!rig.ask(RequestKind::Vote, 3));  // запоздалый запрос прошлого круга — нет
    CHECK(rig.dialog().find("лимит обращений за круг исчерпан") != std::string::npos);
    // Нулевой лимит — ИИ выключен.
    AiBudget zero(0);
    CHECK(!zero.try_acquire(1));
}

void budget_is_atomic_under_contention() {
    // 8 нитей наперегонки занимают обращения: за круг — ровно лимит, ни одним больше.
    // Нити стартуют одновременно (флаг go), попыток много — окно гонки при
    // неатомарной реализации («прочитал — проверил — записал») обязательно
    // проявится: выданных станет больше лимита или больше учтённых.
    for (int round = 1; round <= 5; ++round) {
        constexpr int kLimit = 50'000;
        AiBudget b(kLimit);
        std::atomic<int> granted{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&] {
                while (!go) {
                }
                for (int i = 0; i < 10'000; ++i) {
                    if (b.try_acquire(round)) {
                        ++granted;
                    }
                }
            });
        }
        go = true;
        for (auto& t : ts) {
            t.join();
        }
        CHECK(granted == kLimit);
        CHECK(b.used(round) == kLimit);
    }
    // Смена круга во время гонки: на каждый круг — не больше лимита.
    AiBudget b(5);
    std::map<int, std::atomic<int>> per_round;
    for (int r = 1; r <= 50; ++r) {
        per_round[r];
    }
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            for (int r = 1; r <= 50; ++r) {
                for (int i = 0; i < 3; ++i) {
                    if (b.try_acquire(r)) {
                        ++per_round.at(r);
                    }
                }
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }
    bool within = true;
    for (auto& [r, n] : per_round) {
        within = within && n <= 5;
    }
    CHECK(within);
}

// ============================================================================
// Игрок: совет недопустим или советник падает -> эвристика
// ============================================================================

class ScriptedAdvisor final : public Advisor {
public:
    enum class Mode { Throw, BadTarget, Empty, Good };
    explicit ScriptedAdvisor(Mode m) : mode_(m) {}
    std::optional<AdvisorAnswer> advise(const Request& r) override {
        ++calls;
        switch (mode_) {
            case Mode::Throw:
                throw std::runtime_error("сеть упала");
            case Mode::BadTarget:
                return AdvisorAnswer{PlayerId{77}, "голос в пустоту", std::nullopt};
            case Mode::Empty:
                return AdvisorAnswer{std::nullopt, "", std::nullopt};
            case Mode::Good:
                return AdvisorAnswer{r.targets.empty() ? std::nullopt : std::optional<PlayerId>(r.targets.back()),
                                     "совет", std::nullopt};
        }
        return std::nullopt;
    }
    int calls = 0;

private:
    Mode mode_;
};

void player_falls_back_on_bad_advice() {
    Request r = request(RequestKind::Vote);
    r.you.role = RoleKind::Civilian;
    r.you.known.clear();
    for (auto mode : {ScriptedAdvisor::Mode::Throw, ScriptedAdvisor::Mode::BadTarget, ScriptedAdvisor::Mode::Empty}) {
        SharedPtr<Player> p = PlayerFactory::create(RoleKind::Civilian, PlayerSetup(0, "Игрок 1", Rng(5)));
        auto adv = make_shared_ptr<ScriptedAdvisor>(mode);
        p->set_advisor(adv);
        const Response v = p->handle(r);
        CHECK(adv->calls == 1);
        CHECK(v.target && std::ranges::find(r.targets, *v.target) != r.targets.end());  // эвристика выбрала из списка
        r.kind = RequestKind::Speech;
        CHECK(!p->handle(r).speech.empty());
        r.kind = RequestKind::Vote;
    }
    SharedPtr<Player> p = PlayerFactory::create(RoleKind::Civilian, PlayerSetup(0, "Игрок 1", Rng(5)));
    p->set_advisor(make_shared_ptr<ScriptedAdvisor>(ScriptedAdvisor::Mode::Good));
    const Response v = p->handle(r);
    CHECK(v.target == 4 && v.speech == "совет");
}

// ============================================================================
// Целые партии
// ============================================================================

struct Game {
    std::string out;
    std::map<PlayerId, RoleKind> roles;
};

Game play(std::uint64_t seed, int ai, const std::string& url, SharedPtr<AiDialogLog> log, bool coro,
          int limit = 10) {
    Game g;
    HostConfig c;
    c.players = 9;
    c.seed = seed;
    c.make_player = [&g](RoleKind role, PlayerSetup s) {
        g.roles[s.id] = role;
        return PlayerFactory::create(role, std::move(s));
    };
    if (ai > 0) {
        auto cfg = make_shared_ptr<AiConfig>();
        cfg->base_url = url;
        cfg->api_key = kKey;
        cfg->timeout = std::chrono::milliseconds(3000);
        const SharedPtr<const AiConfig> config = cfg;
        const SharedPtr<HttpClient> client = make_shared_ptr<CurlClient>();
        const auto budget = make_shared_ptr<AiBudget>(limit);
        c.ai_players = ai;
        c.make_advisor = [=](PlayerId, int k) -> SharedPtr<Advisor> {
            return make_shared_ptr<AiAdvisor>(config, client, budget, log, std::string(personality_for(k)));
        };
    }
    std::ostringstream out;
    SharedPtr<Engine> engine;
    if (coro) {
        engine = make_shared_ptr<CoroEngine>();
    } else {
        engine = make_shared_ptr<ThreadEngine>();
    }
    Host host(c, engine, out);
    host.setup();
    host.play();
    host.shutdown();
    g.out = out.str();
    return g;
}

std::string result_line(const std::string& out) {
    const auto pos = out.find("RESULT ");
    return pos == std::string::npos ? std::string() : out.substr(pos, out.find('\n', pos) - pos);
}

void full_games_with_ai_players() {
    for (bool coro : {false, true}) {
        fake::Server server;
        auto log = make_shared_ptr<AiDialogLog>(temp_dir(coro ? "game_coro" : "game_threads") / "ai_dialog.txt");
        const Game g = play(2026, 3, server.url(), log, coro);
        CHECK(!result_line(g.out).empty());
        CHECK(g.out.find("ИИ-агенты:") != std::string::npos);
        CHECK(server.count() > 0);
        CHECK(g.out.find("ведёт себя подозрительно") != std::string::npos);  // реплики ИИ дошли до стола
        CHECK(g.out.find(kKey) == std::string::npos);

        // Ни в одном запросе нет чужих тайн: тайный раздел есть только у тех,
        // кому ведущий что-то сообщил, и он описывает лишь им известное.
        bool clean = true;
        for (const fake::Received& r : server.received()) {
            const json::Value v = json::parse(r.body);
            const json::Array& msgs = *v.get("messages")->as_array();
            const std::string system = *msgs[0].get("content")->as_string();
            const std::string user = *msgs[1].get("content")->as_string();
            const auto at = user.find("Ты — Игрок ");
            const PlayerId self = std::stoi(user.substr(at + std::string("Ты — Игрок ").size())) - 1;
            const RoleKind role = g.roles.at(self);
            if (role == RoleKind::Civilian && system.find("ничего сверх публичного") == std::string::npos) {
                clean = false;
            }
            if (team_of(role) != Team::Mafia && system.find("мафия/враг (Мафиози)") != std::string::npos &&
                role != RoleKind::Commissar) {
                clean = false;  // о союзниках мафии знает только мафия (и комиссар — о проверенных)
            }
            if (user.find("мафия/враг") != std::string::npos || r.body.find(kKey) != std::string::npos) {
                clean = false;
            }
        }
        CHECK(clean);
        CHECK(read_file(log->path()).find(kKey) == std::string::npos);
    }
}

void game_with_dead_service_equals_plain_game() {
    // Сервис недоступен на каждом обращении: все ИИ-игроки решают эвристикой,
    // и партия совпадает с обычной партией с тем же зерном.
    std::string dead;
    {
        fake::Server gone;
        dead = gone.url();
    }
    auto log = make_shared_ptr<AiDialogLog>(temp_dir("dead") / "ai_dialog.txt");
    const Game ai = play(77, 4, dead, log, false);
    const Game plain = play(77, 0, "", SharedPtr<AiDialogLog>(), false);
    CHECK(!result_line(ai.out).empty());
    CHECK(result_line(ai.out) == result_line(plain.out));
    // Весь протокол, кроме строки об ИИ-агентах.
    std::string stripped = ai.out;
    const auto from = stripped.find("ИИ-агенты:");
    stripped.erase(from, stripped.find('\n', from) - from + 1);
    CHECK(stripped == plain.out);
    CHECK(read_file(log->path()).find("не удалось подключиться") != std::string::npos);
}

void game_with_flaky_service_finishes() {
    // Каждый ответ — новый вид сбоя, по кругу; лимит маленький.
    fake::Server server;
    for (int i = 0; i < 40; ++i) {
        switch (i % 6) {
            case 0: server.push(fake::Script::reply(500, "{}")); break;
            case 1: server.push(fake::Script::content("не JSON")); break;
            case 2: server.push(fake::Script::close()); break;
            case 3: server.push(fake::Script::content(R"({"action":"vote","target":99,"reasoning":"","message":""})")); break;
            case 4: server.push(fake::Script::garbage()); break;
            default: server.push(fake::Script::smart()); break;
        }
    }
    auto log = make_shared_ptr<AiDialogLog>(temp_dir("flaky") / "ai_dialog.txt");
    const Game g = play(5, 5, server.url(), log, false, /*limit=*/4);
    CHECK(!result_line(g.out).empty());
    const std::string d = read_file(log->path());
    CHECK(d.find("[отвергнут]") != std::string::npos);
    CHECK(d.find("[сбой]") != std::string::npos);
    CHECK(d.find("лимит обращений за круг исчерпан") != std::string::npos);
}

}  // namespace

int main() {
    test::run("JSON: корректные документы", &json_parses_valid_documents);
    test::run("JSON: ошибки и глубина", &json_rejects_malformed_documents);
    test::run("запрос: системная часть и сообщение", &prompt_splits_system_and_message);
    test::run("запрос: без чужих тайн", &prompt_has_no_secrets_of_others);
    test::run("ответ: успех каждого вида", &reply_success_for_every_kind);
    test::run("ответ: обрамление и очистка текста", &reply_tolerates_wrapping_and_cleans_text);
    test::run("ответ: все виды отказа", &reply_rejections);
    test::run("сервер: OpenAI и ключ", &server_success_openai_and_key_handling);
    test::run("сервер: Anthropic", &server_success_anthropic);
    test::run("сервер: без ключа", &no_key_means_no_auth_header);
    test::run("сервер: коды ошибок и плохие ответы", &server_http_errors);
    test::run("сервер: таймаут, обрыв, мусор, отказ", &server_transport_failures);
    test::run("лимит: за круг", &budget_limits_calls_per_round);
    test::run("лимит: атомарность", &budget_is_atomic_under_contention);
    test::run("игрок: откат на эвристику", &player_falls_back_on_bad_advice);
    test::run("партии с ИИ на обоих движках", &full_games_with_ai_players);
    test::run("недоступный сервис = обычная партия", &game_with_dead_service_equals_plain_game);
    test::run("партия при сбоях всех видов", &game_with_flaky_service_finishes);
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / ("mafia_test_ai_" + std::to_string(::getpid())));
    return test::finish();
}
