#include "cli/options.hpp"

#include <charconv>
#include <limits>
#include <set>

namespace mafia {

namespace {

// Целое из строки: вся строка — число, без знака «+», пробелов и хвостов.
// from_chars не знает локалей и не бросает исключений — всё проверяем сами.
template <class T>
T parse_number(std::string_view flag, std::string_view text) {
    T value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    if (text.empty()) {
        throw OptionsError("у параметра " + std::string(flag) + " пустое значение");
    }
    if (std::numeric_limits<T>::is_signed == false && text.front() == '-') {
        throw OptionsError("значение " + std::string(flag) + " не может быть отрицательным: «" + std::string(text) + "»");
    }
    const auto [end, error] = std::from_chars(first, last, value);
    if (error == std::errc::result_out_of_range) {
        throw OptionsError("значение " + std::string(flag) + " слишком велико: «" + std::string(text) + "»");
    }
    if (error != std::errc{} || end != last) {
        throw OptionsError("значение " + std::string(flag) + " должно быть целым числом, а не «" + std::string(text) + "»");
    }
    return value;
}

}  // namespace

Options parse_options(const std::vector<std::string_view>& args) {
    Options o;
    std::set<std::string_view> seen;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (!arg.starts_with("--")) {
            throw OptionsError("непонятный аргумент «" + std::string(arg) + "» — параметры начинаются с --");
        }
        // --name=value или --name value
        std::string_view flag = arg;
        std::optional<std::string_view> inline_value;
        if (const auto eq = arg.find('='); eq != std::string_view::npos) {
            flag = arg.substr(0, eq);
            inline_value = arg.substr(eq + 1);
        }
        if (!seen.insert(flag).second) {
            throw OptionsError("параметр " + std::string(flag) + " указан дважды");
        }

        auto value = [&]() -> std::string_view {
            if (inline_value) {
                return *inline_value;
            }
            if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
                throw OptionsError("после " + std::string(flag) + " нужно значение");
            }
            return args[++i];
        };
        auto no_value = [&] {
            if (inline_value) {
                throw OptionsError("параметр " + std::string(flag) + " не принимает значения");
            }
        };

        if (flag == "--players") {
            o.players = parse_number<int>(flag, value());
            if (o.players <= 4) {
                throw OptionsError("игроков должно быть больше 4, задано " + std::to_string(o.players));
            }
            if (o.players > 300) {
                throw OptionsError("игроков не больше 300, задано " + std::to_string(o.players));
            }
        } else if (flag == "--k") {
            o.mafia_divisor = parse_number<int>(flag, value());
            o.divisor_given = true;
            if (o.mafia_divisor < 3) {
                throw OptionsError("делитель k должен быть не меньше 3, задано " + std::to_string(o.mafia_divisor));
            }
        } else if (flag == "--seed") {
            o.seed = parse_number<std::uint64_t>(flag, value());
        } else if (flag == "--engine") {
            const std::string_view e = value();
            if (e == "threads") {
                o.engine = EngineKind::Threads;
            } else if (e == "coro") {
                o.engine = EngineKind::Coroutines;
            } else {
                throw OptionsError("движок «" + std::string(e) + "» неизвестен — возможны threads или coro");
            }
        } else if (flag == "--full-log") {
            no_value();
            o.full_log = true;
        } else if (flag == "--log-dir") {
            const std::string_view dir = value();
            if (dir.empty()) {
                throw OptionsError("каталог журналов не может быть пустым");
            }
            o.log_dir = std::string(dir);
        } else if (flag == "--config") {
            const std::string_view file = value();
            if (file.empty()) {
                throw OptionsError("путь к конфигу не может быть пустым");
            }
            o.config = std::string(file);
        } else if (flag == "--interactive") {
            no_value();
            o.interactive = true;
        } else if (flag == "--seat") {
            o.seat = parse_number<int>(flag, value());
            if (*o.seat < 1) {
                throw OptionsError("место за столом — с 1, задано " + std::to_string(*o.seat));
            }
        } else if (flag == "--open-announcements") {
            no_value();
            o.open_announcements = true;
        } else if (flag == "--ai") {
            o.ai_players = parse_number<int>(flag, value());
            if (o.ai_players < 0) {
                throw OptionsError("число ИИ-игроков не может быть отрицательным: " + std::to_string(o.ai_players));
            }
        } else if (flag == "--ai-url") {
            const std::string_view url = value();
            if (!url.starts_with("http://") && !url.starts_with("https://")) {
                throw OptionsError("адрес API должен начинаться с http:// или https://, а не «" + std::string(url) + "»");
            }
            o.ai_url = std::string(url);
        } else if (flag == "--ai-model") {
            const std::string_view model = value();
            if (model.empty()) {
                throw OptionsError("имя модели не может быть пустым");
            }
            o.ai_model = std::string(model);
        } else if (flag == "--ai-format") {
            const std::string_view f = value();
            if (f != "openai" && f != "anthropic") {
                throw OptionsError("формат API «" + std::string(f) + "» неизвестен — возможны openai или anthropic");
            }
            o.ai_anthropic = f == "anthropic";
        } else if (flag == "--ai-limit") {
            o.ai_limit = parse_number<int>(flag, value());
            if (o.ai_limit < 0 || o.ai_limit > 1000) {
                throw OptionsError("лимит обращений за круг — от 0 до 1000, задано " + std::to_string(o.ai_limit));
            }
        } else if (flag == "--ai-timeout") {
            o.ai_timeout = parse_number<int>(flag, value());
            if (o.ai_timeout < 1 || o.ai_timeout > 120) {
                throw OptionsError("ожидание ответа — от 1 до 120 секунд, задано " + std::to_string(o.ai_timeout));
            }
        } else if (flag == "--ai-key" || flag == "--api-key" || flag == "--key") {
            // Значение не читаем и не повторяем в сообщении.
            throw OptionsError("ключ API в командной строке не принимается — задайте переменную окружения MAFIA_AI_KEY");
        } else if (flag == "--help") {
            no_value();
            o.help = true;
        } else {
            throw OptionsError("неизвестный параметр " + std::string(flag));
        }
    }
    // Проверки, которым нужны несколько параметров сразу, — после разбора всех.
    if (o.seat && !o.interactive) {
        throw OptionsError("--seat задаёт место пользователя — он нужен только вместе с --interactive");
    }
    if (o.seat && *o.seat > o.players) {
        throw OptionsError("место " + std::to_string(*o.seat) + " — а игроков всего " + std::to_string(o.players));
    }
    const int bots = o.players - (o.interactive ? 1 : 0);  // место пользователя ИИ не отдаётся
    if (o.ai_players > bots) {
        throw OptionsError("ИИ-игроков " + std::to_string(o.ai_players) + " — больше, чем мест для ботов (" +
                           std::to_string(bots) + ")");
    }
    return o;
}

std::string usage(std::string_view program) {
    return "Использование: " + std::string(program) +
           " [параметры]\n"
           "  --players N      число игроков, от 5 до 300 (по умолчанию 8); при N > 100 протокол\n"
           "                   пишется только в журнал, в консоли — прогресс по дням и итог\n"
           "  --k K            мафии floor(N/K), но не меньше 1; K >= 3 (по умолчанию 3)\n"
           "  --seed S         зерно партии (по умолчанию — случайное, печатается)\n"
           "  --engine E       threads — нить на игрока, coro — корутины (по умолчанию threads)\n"
           "  --full-log       полный журнал в файлах: с тайными ходами (в консоль они не попадают)\n"
           "  --log-dir PATH   каталог журналов (по умолчанию logs)\n"
           "  --config FILE    YAML-конфиг: k и список дополнительных ролей (--k важнее)\n"
           "  --interactive    вы — один из N игроков: ведущий подсказывает фазу и спрашивает ход\n"
           "  --seat S         ваше место за столом, 1..N (по умолчанию — по зерну; только с --interactive)\n"
           "  --open-announcements\n"
           "                   открытые объявления: роль выбывшего, кто убил, кого спасли\n"
           "                   (по умолчанию закрытые: только «мафия / не мафия»)\n"
           "  --ai N           сколько игроков ведёт ИИ-агент (по умолчанию 0)\n"
           "  --ai-url URL     адрес API; по умолчанию MAFIA_AI_URL или https://vireonix.ai/v1\n"
           "  --ai-model NAME  модель (по умолчанию auto)\n"
           "  --ai-format F    openai — /chat/completions, anthropic — /messages (по умолчанию openai)\n"
           "  --ai-limit L     обращений к модели за круг на всех ИИ-игроков (по умолчанию 10)\n"
           "  --ai-timeout S   ожидание ответа модели, секунд (по умолчанию 20)\n"
           "                   Ключ API — только в переменной окружения MAFIA_AI_KEY.\n"
           "  --help           эта справка\n"
           "Пример: " + std::string(program) + " --players 10 --interactive --open-announcements --full-log\n";
}

}  // namespace mafia
