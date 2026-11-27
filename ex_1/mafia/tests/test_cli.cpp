// Тесты разбора командной строки: значения по умолчанию, обе формы записи,
// и каждая ошибка — понятное сообщение, а не падение и не тихое «как-нибудь».
#include "cli/options.hpp"
#include "test_framework.hpp"

#include <string>

using namespace mafia;

namespace {

Options parse(std::vector<std::string_view> args) {
    return parse_options(args);
}

// Ошибка ожидается; возвращаем её текст (пусто — ошибки не было).
std::string error_of(std::vector<std::string_view> args) {
    try {
        parse_options(args);
    } catch (const OptionsError& e) {
        return e.what();
    }
    return {};
}

bool mentions(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

void defaults() {
    const Options o = parse({});
    CHECK(o.players == 8 && o.mafia_divisor == 3 && !o.seed);
    CHECK(o.engine == EngineKind::Threads && !o.full_log && o.log_dir == "logs" && !o.help);
}

void all_options() {
    const Options o = parse({"--players", "12", "--k=4", "--seed", "18446744073709551615", "--engine", "coro",
                             "--full-log", "--log-dir=/tmp/x", "--help"});
    CHECK(o.players == 12 && o.mafia_divisor == 4);
    CHECK(o.seed == 18446744073709551615ULL);  // 2^64 − 1 — ещё допустимо
    CHECK(o.engine == EngineKind::Coroutines && o.full_log && o.log_dir == "/tmp/x" && o.help);
    CHECK(parse({"--engine=threads"}).engine == EngineKind::Threads);
}

void bad_numbers() {
    CHECK(mentions(error_of({"--players", "-3"}), "больше 4"));
    CHECK(mentions(error_of({"--players", "4"}), "больше 4"));
    CHECK(mentions(error_of({"--players", "десять"}), "целым числом"));
    CHECK(mentions(error_of({"--players", "10abc"}), "целым числом"));
    CHECK(mentions(error_of({"--players", "+10"}), "целым числом"));
    CHECK(mentions(error_of({"--players", "99999999999"}), "слишком велико"));
    CHECK(mentions(error_of({"--players", "301"}), "не больше 300"));
    CHECK(mentions(error_of({"--players="}), "пустое"));
    CHECK(mentions(error_of({"--k", "2"}), "не меньше 3"));
    CHECK(mentions(error_of({"--seed", "-1"}), "отрицательным"));
    CHECK(mentions(error_of({"--seed", "18446744073709551616"}), "слишком велико"));  // 2^64
}

void bad_structure() {
    CHECK(mentions(error_of({"--players"}), "нужно значение"));
    CHECK(mentions(error_of({"--players", "--seed", "1"}), "нужно значение"));
    CHECK(mentions(error_of({"--engine", "fibers"}), "threads или coro"));
    CHECK(mentions(error_of({"--interactive=yes"}), "не принимает значения"));
    CHECK(mentions(error_of({"players"}), "начинаются с --"));
    CHECK(mentions(error_of({"--seed", "1", "--seed", "2"}), "дважды"));
    CHECK(mentions(error_of({"--full-log=yes"}), "не принимает значения"));
    CHECK(mentions(error_of({"--log-dir", ""}), "пуст"));
}

void interactive_options() {
    const Options d = parse({});
    CHECK(!d.interactive && !d.seat && !d.open_announcements);
    const Options o = parse({"--players", "10", "--interactive", "--seat", "4", "--open-announcements"});
    CHECK(o.interactive && o.seat == 4 && o.open_announcements);
    CHECK(parse({"--interactive"}).interactive && !parse({"--interactive"}).seat);  // место — по зерну
    CHECK(mentions(error_of({"--seat", "2"}), "только вместе с --interactive"));
    CHECK(mentions(error_of({"--interactive", "--seat", "0"}), "с 1"));
    CHECK(mentions(error_of({"--interactive", "--seat", "9"}), "игроков всего 8"));
    CHECK(mentions(error_of({"--interactive", "--seat", "x"}), "целым числом"));
    CHECK(mentions(error_of({"--open-announcements=1"}), "не принимает значения"));
}

void usage_lists_everything() {
    const std::string u = usage("mafia");
    for (const char* flag : {"--players", "--k", "--seed", "--engine", "--full-log", "--log-dir", "--config", "--ai",
                             "--ai-url", "--ai-model", "--ai-format", "--ai-limit", "--ai-timeout", "MAFIA_AI_KEY",
                             "--interactive", "--seat", "--open-announcements", "--help"}) {
        CHECK(mentions(u, flag));
    }
}

void ai_options() {
    const Options d = parse({});
    CHECK(d.ai_players == 0 && !d.ai_url && !d.ai_model && !d.ai_anthropic && d.ai_limit == 10 && d.ai_timeout == 20);
    const Options o = parse({"--players", "9", "--ai", "3", "--ai-url=http://127.0.0.1:8089/v1", "--ai-model", "m",
                             "--ai-format", "anthropic", "--ai-limit", "0", "--ai-timeout=5"});
    CHECK(o.ai_players == 3 && o.ai_url == "http://127.0.0.1:8089/v1" && o.ai_model == "m");
    CHECK(o.ai_anthropic && o.ai_limit == 0 && o.ai_timeout == 5);

    CHECK(mentions(error_of({"--ai", "-1"}), "отрицательным"));
    CHECK(mentions(error_of({"--ai", "9"}), "больше, чем мест для ботов"));
    CHECK(mentions(error_of({"--ai", "8", "--interactive"}), "больше, чем мест для ботов"));  // место пользователя не ИИ
    CHECK(mentions(error_of({"--ai-url", "file:///etc/passwd"}), "http://"));
    CHECK(mentions(error_of({"--ai-format", "gemini"}), "openai или anthropic"));
    CHECK(mentions(error_of({"--ai-limit", "-5"}), "от 0 до 1000"));
    CHECK(mentions(error_of({"--ai-timeout", "0"}), "от 1 до 120"));
    CHECK(!error_of({"--ai-model="}).empty());
    // Ключ в командной строке не принимается — и в сообщении об ошибке его нет.
    for (std::vector<std::string_view> args : {std::vector<std::string_view>{"--ai-key", "sk-SECRET"},
                                               std::vector<std::string_view>{"--api-key=sk-SECRET"},
                                               std::vector<std::string_view>{"--key", "sk-SECRET"}}) {
        const std::string e = error_of(args);
        CHECK(mentions(e, "MAFIA_AI_KEY"));
        CHECK(!mentions(e, "SECRET"));
    }
}

int main() {
    std::cout << "Командная строка\n";
    test::run("значения по умолчанию", &defaults);
    test::run("все параметры, обе формы записи", &all_options);
    test::run("неверные числа", &bad_numbers);
    test::run("неверная структура", &bad_structure);
    test::run("параметры ИИ-агента", &ai_options);
    test::run("пользователь за столом и объявления", &interactive_options);
    test::run("справка перечисляет все параметры", &usage_lists_everything);
    return test::finish();
}
