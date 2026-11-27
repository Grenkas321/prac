// Параметры командной строки.
//
//   --players N        число игроков, 4 < N <= 300          (по умолчанию 8)
//   --k K              делитель числа мафии, K >= 3          (по умолчанию 3)
//   --seed S           зерно партии, 0 <= S < 2^64           (по умолчанию — случайное)
//   --engine E         threads | coro                        (по умолчанию threads)
//   --full-log         полный журнал в файлах (с тайным)     (по умолчанию краткий)
//   --log-dir PATH     каталог журналов                      (по умолчанию logs)
//   --config FILE      YAML-конфиг: k и дополнительные роли   (--k важнее k из конфига)
//   --interactive      пользователь — один из N игроков       (по умолчанию все — боты)
//   --seat S           место пользователя, 1..N               (по умолчанию — по зерну)
//   --open-announcements  открытые объявления: роли выбывших,  (по умолчанию закрытые)
//                      кто кого убил, кого спасли
//   --ai N             сколько игроков ведёт ИИ-агент         (по умолчанию 0)
//   --ai-url URL       адрес API (или MAFIA_AI_URL)           (по умолчанию https://vireonix.ai/v1)
//   --ai-model NAME    модель                                 (по умолчанию auto)
//   --ai-format F      openai | anthropic                     (по умолчанию openai)
//   --ai-limit L       обращений к модели за круг, L >= 0     (по умолчанию 10)
//   --ai-timeout S     ожидание ответа, секунд, 1..120        (по умолчанию 20)
//
// Ключа API среди параметров нет и не будет: он берётся только из переменной
// окружения MAFIA_AI_KEY. Командная строка видна всем (ps, история оболочки).
//
//   --help             справка
//
// Значение пишется через пробел или через «=»: --players 10, --players=10.
// Любая ошибка — исключение OptionsError с понятным сообщением.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mafia {

enum class EngineKind { Threads, Coroutines };

struct Options {
    int players = 8;
    int mafia_divisor = 3;
    bool divisor_given = false;          // --k указан явно (важнее k из конфига)
    std::optional<std::string> config;  // --config: YAML с k и дополнительными ролями
    std::optional<std::uint64_t> seed;  // пусто — выбрать случайно и напечатать
    EngineKind engine = EngineKind::Threads;
    bool full_log = false;
    std::string log_dir = "logs";
    bool help = false;

    // Пользователь за столом и вид объявлений ведущего.
    bool interactive = false;
    std::optional<int> seat;  // 1..N; пусто — выбрать по зерну
    bool open_announcements = false;

    // ИИ-агент (этап 16). Пустые url/model — значения по умолчанию из AiConfig.
    int ai_players = 0;
    std::optional<std::string> ai_url;
    std::optional<std::string> ai_model;
    bool ai_anthropic = false;
    int ai_limit = 10;
    int ai_timeout = 20;
};

class OptionsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// args — аргументы без имени программы (argv[1..]).
Options parse_options(const std::vector<std::string_view>& args);

std::string usage(std::string_view program);

}  // namespace mafia
