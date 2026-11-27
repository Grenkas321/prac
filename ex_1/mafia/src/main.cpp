// Точка входа программы «Мафия».
//
//   ./mafia --players 10 --seed 2026 --engine coro --full-log
//
// Ошибка в параметрах — сообщение, подсказка и код выхода 2.
// Исключение из любой нити игрока движок перебрасывает в главную нить,
// оно долетает сюда, деструкторы по пути останавливают нити, код выхода — 1.
#include "ai/ai_advisor.hpp"
#include "ai/http_client.hpp"
#include "cli/config.hpp"
#include "cli/options.hpp"
#include "engine/console_engine.hpp"
#include "engine/coro_engine.hpp"
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "log/game_log.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

namespace {

// Больше стольких игроков (и не --interactive) — тихий режим: протокол только в журнал.
constexpr int kQuietAbove = 100;

// Настройки ИИ из параметров и окружения. Ключ — только из MAFIA_AI_KEY.
mafia::AiConfig ai_config(const mafia::Options& options) {
    mafia::AiConfig ai;
    if (options.ai_url) {
        ai.base_url = *options.ai_url;
    } else if (const char* env = std::getenv("MAFIA_AI_URL"); env != nullptr && *env != '\0') {
        const std::string url = env;
        if (!url.starts_with("http://") && !url.starts_with("https://")) {
            throw std::invalid_argument("MAFIA_AI_URL должен начинаться с http:// или https://");
        }
        ai.base_url = url;
    }
    while (ai.base_url.ends_with('/')) {
        ai.base_url.pop_back();  // к адресу добавляется /chat/completions или /messages
    }
    if (options.ai_model) {
        ai.model = *options.ai_model;
    }
    ai.format = options.ai_anthropic ? mafia::AiFormat::Anthropic : mafia::AiFormat::OpenAI;
    ai.calls_per_round = options.ai_limit;
    ai.timeout = std::chrono::seconds(options.ai_timeout);
    ai.api_key = mafia::AiConfig::key_from_environment();
    return ai;
}

int run(const mafia::Options& options) {
    mafia::HostConfig config;
    config.players = options.players;
    config.mafia_divisor = options.mafia_divisor;
    if (options.config) {
        // Из конфига: дополнительные роли и k (если --k не задан явно).
        const mafia::GameConfig file = mafia::load_config(*options.config);
        config.extra_roles = file.extra_roles;
        if (file.mafia_divisor && !options.divisor_given) {
            config.mafia_divisor = *file.mafia_divisor;
        }
    }
    // Без --seed — случайное зерно; ведущий печатает его в начале партии,
    // так что любую партию можно повторить.
    config.seed = options.seed ? *options.seed : (std::uint64_t{std::random_device{}()} << 32) ^ std::random_device{}();
    config.log = mafia::make_shared_ptr<mafia::GameLog>(
        options.log_dir, config.seed, options.full_log ? mafia::GameLog::Level::Full : mafia::GameLog::Level::Brief);

    config.open_announcements = options.open_announcements;

    mafia::SharedPtr<mafia::Engine> engine;
    if (options.engine == mafia::EngineKind::Coroutines) {
        engine = mafia::make_shared_ptr<mafia::CoroEngine>();
    } else {
        engine = mafia::make_shared_ptr<mafia::ThreadEngine>();
    }
    if (options.interactive) {
        // Место пользователя: --seat или по зерну — своим потоком n + 2
        // (n + 1 — места ИИ), чтобы раздача и ходы ботов не менялись.
        const mafia::PlayerId seat =
            options.seat ? *options.seat - 1
                         : static_cast<mafia::PlayerId>(
                               mafia::Rng::stream(config.seed, static_cast<std::uint64_t>(options.players) + 2)
                                   .below(static_cast<std::uint64_t>(options.players)));
        config.human_seat = seat;
        engine = mafia::make_shared_ptr<mafia::ConsoleEngine>(engine, seat, std::cin, std::cout);
    }

    mafia::SharedPtr<mafia::AiDialogLog> dialog;
    if (options.ai_players > 0) {
        // Настройки после создания только читаются — отсюда const.
        const mafia::SharedPtr<const mafia::AiConfig> ai = mafia::make_shared_ptr<mafia::AiConfig>(ai_config(options));
        // Клиент, бюджет и журнал — общие на всех ИИ-игроков: лимит — на круг, а не на игрока.
        const mafia::SharedPtr<mafia::HttpClient> client = mafia::make_shared_ptr<mafia::CurlClient>();
        const auto budget = mafia::make_shared_ptr<mafia::AiBudget>(ai->calls_per_round);
        dialog = mafia::make_shared_ptr<mafia::AiDialogLog>(config.log->dir() / "ai_dialog.txt");
        config.ai_players = options.ai_players;
        config.make_advisor = [=](mafia::PlayerId, int index) -> mafia::SharedPtr<mafia::Advisor> {
            return mafia::make_shared_ptr<mafia::AiAdvisor>(ai, client, budget, dialog,
                                                            std::string(mafia::personality_for(index)));
        };
    }

    // Большая партия: протокол на тысячи строк в консоли никто не прочтёт —
    // он идёт только в журнал, а в консоль — прогресс (точка на день) и итог.
    // С пользователем за столом — никогда: ему нужен протокол.
    config.quiet = options.players > kQuietAbove && !options.interactive;

    auto print_dialog = [&] {
        if (dialog) {
            // Сам ключ не печатается никогда — только задан он или нет.
            std::cout << "Диалог с моделью: " << dialog->path().string() << "; ключ API: "
                      << (mafia::AiConfig::key_from_environment().empty() ? "не задан" : "задан (MAFIA_AI_KEY)")
                      << "\n";
        }
    };
    if (config.quiet) {
        print_dialog();  // до строки прогресса, чтобы не разорвать её
        std::cout << "Да, это будет долго " << std::flush;
    }
    mafia::Host host(config, engine, std::cout);
    host.setup();
    if (!config.quiet) {
        print_dialog();
    }
    host.play();  // до победы одной из сторон, ничьей или предела дней; печатает итог
    host.shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view program = argc > 0 ? argv[0] : "mafia";
    mafia::Options options;
    try {
        options = mafia::parse_options(std::vector<std::string_view>(argv + 1, argv + argc));
    } catch (const mafia::OptionsError& e) {
        std::cerr << "Ошибка в параметрах: " << e.what() << "\n\n" << mafia::usage(program);
        return 2;
    }
    if (options.help) {
        std::cout << mafia::usage(program);
        return 0;
    }

    try {
        return run(options);
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "Ошибка: неизвестное исключение\n";
    }
    return 1;
}
