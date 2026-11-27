// Тесты журналов партии (std::filesystem): каталоги и файлы создаются,
// тайное — только в полном журнале и никогда в консоли, повторный запуск
// в ту же секунду не затирает предыдущий.
#include "engine/thread_engine.hpp"
#include "game/host.hpp"
#include "log/game_log.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mafia;

namespace {

// Свой временный корень на каждый прогон; удаляется в конце.
fs::path test_root() {
    static const fs::path root = fs::temp_directory_path() / "mafia_test_logs";
    return root;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

struct Played {
    GameState state;
    std::string console;
    fs::path dir;
};

Played play(GameLog::Level level, std::uint64_t seed, bool quiet = false) {
    HostConfig c;
    c.players = 8;
    c.seed = seed;
    c.quiet = quiet;
    c.log = make_shared_ptr<GameLog>(test_root(), seed, level);
    std::ostringstream console;
    Host host(c, make_shared_ptr<ThreadEngine>(), console);
    host.setup();
    host.play();
    host.shutdown();
    return Played{host.state(), console.str(), c.log->dir()};
}

// Файлы кругов по порядку, склеенные; тайные строки можно выбросить.
std::string rounds_text(const fs::path& dir, int rounds, bool drop_secret) {
    std::string out;
    for (int r = 1; r <= rounds; ++r) {
        char name[32];
        std::snprintf(name, sizeof(name), "round_%02d.txt", r);
        for (const std::string& line : lines(read_file(dir / name))) {
            if (drop_secret && line.starts_with(GameLog::kSecretMark)) {
                continue;
            }
            out += line + "\n";
        }
    }
    return out;
}

}  // namespace

void files_are_created() {
    fs::remove_all(test_root());
    const Played g = play(GameLog::Level::Full, 2026);

    CHECK(fs::is_directory(g.dir));
    CHECK(g.dir.parent_path() == test_root());
    // Имя каталога: дата_время_seedЗЕРНО.
    CHECK(std::regex_match(g.dir.filename().string(),
                           std::regex(R"(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_seed2026)")));

    // Файл на каждый круг — ровно столько, сколько дней.
    auto rounds = fs::directory_iterator(g.dir) | std::views::filter([](const fs::directory_entry& e) {
                      return e.path().filename().string().starts_with("round_");
                  });
    CHECK(std::ranges::distance(rounds) == g.state.day);
    CHECK(fs::is_regular_file(g.dir / "round_01.txt"));

    const std::string summary = read_file(g.dir / "summary.txt");
    CHECK(summary.find("=== Итог партии ===") != std::string::npos);
    CHECK(summary.find("Статистика по игрокам") != std::string::npos);
    CHECK(summary.find("RESULT winner=") != std::string::npos);

    const std::vector<std::string> history = lines(read_file(test_root() / "history.txt"));
    CHECK(history.size() == 1);
    CHECK(history.front().starts_with("RESULT winner="));
    CHECK(history.front().ends_with("dir=" + g.dir.filename().string()));
}

void secrets_only_in_full_log() {
    fs::remove_all(test_root());
    const Played full = play(GameLog::Level::Full, 7);
    const Played brief = play(GameLog::Level::Brief, 7);

    // В консоли нет пометки тайного — ни в полном, ни в кратком режиме.
    CHECK(full.console.find(GameLog::kSecretMark) == std::string::npos);
    CHECK(brief.console.find(GameLog::kSecretMark) == std::string::npos);
    // ...и нет самих тайн: ночных ходов и раздачи ролей до конца партии.
    for (const char* secret : {"предлагает:", "лечит ", "проверяет ", "решение мафии"}) {
        CHECK(full.console.find(secret) == std::string::npos);
    }

    // Полный журнал содержит тайное, краткий — нет.
    const std::string full_rounds = rounds_text(full.dir, full.state.day, /*drop_secret=*/false);
    const std::string brief_rounds = rounds_text(brief.dir, brief.state.day, false);
    CHECK(full_rounds.find(GameLog::kSecretMark) != std::string::npos);
    CHECK(full_rounds.find("лечит ") != std::string::npos);
    CHECK(brief_rounds.find(GameLog::kSecretMark) == std::string::npos);

    // Публичная часть журнала — ровно то, что было в консоли, в том же порядке.
    CHECK(rounds_text(full.dir, full.state.day, /*drop_secret=*/true) == full.console);
    CHECK(brief_rounds == brief.console);

    // Каждая тайная строка помечена: без пометки тайн в файлах нет.
    for (const std::string& line : lines(full_rounds)) {
        if (line.find("лечит ") != std::string::npos || line.find("предлагает:") != std::string::npos) {
            CHECK(line.starts_with(GameLog::kSecretMark));
        }
    }
}

void same_seed_same_second() {
    fs::remove_all(test_root());
    // Один и тот же момент и одно зерно — как два запуска в одну секунду.
    const auto moment = std::chrono::system_clock::now();
    GameLog a(test_root(), 42, GameLog::Level::Brief, moment);
    GameLog b(test_root(), 42, GameLog::Level::Brief, moment);
    GameLog c(test_root(), 42, GameLog::Level::Brief, moment);
    CHECK(a.dir() != b.dir() && b.dir() != c.dir());
    CHECK(b.dir().filename().string() == a.dir().filename().string() + "_2");
    CHECK(c.dir().filename().string() == a.dir().filename().string() + "_3");
    CHECK(fs::is_directory(a.dir()) && fs::is_directory(b.dir()) && fs::is_directory(c.dir()));

    a.append_history("RESULT первая");
    b.append_history("RESULT вторая");
    CHECK(lines(read_file(test_root() / "history.txt")).size() == 2);  // дописывается, не затирается
}

void nested_root_is_created() {
    fs::remove_all(test_root());
    const fs::path deep = test_root() / "a" / "b" / "c";
    GameLog log(deep, 1, GameLog::Level::Brief);
    CHECK(fs::is_directory(deep));
    CHECK(log.dir().parent_path() == deep);
    fs::remove_all(test_root());
    CHECK(!fs::exists(test_root()));
}

// Тихий режим (большие партии): в консоли — точка на каждый день и короткий
// итог; протокол в журнале — тот же, что в обычном режиме.
void quiet_mode_console_and_files() {
    fs::remove_all(test_root());
    const Played loud = play(GameLog::Level::Full, 77);
    const Played quiet = play(GameLog::Level::Full, 77, /*quiet=*/true);
    const int days = quiet.state.day;
    CHECK(days >= 1);

    const std::vector<std::string> out = lines(quiet.console);
    CHECK(!out.empty() && out[0] == std::string(static_cast<std::size_t>(days), '.'));  // точка на день
    CHECK(quiet.console.find("=== День") == std::string::npos);  // протокола в консоли нет
    CHECK(quiet.console.find("Дней сыграно: " + std::to_string(days)) != std::string::npos);
    CHECK(quiet.console.find("RESULT ") != std::string::npos);
    CHECK(quiet.console.find(quiet.dir.string()) != std::string::npos);  // где искать протокол
    CHECK(lines(quiet.console).size() <= 6);                              // коротко, без таблицы игроков
    CHECK(quiet.console.find(GameLog::kSecretMark) == std::string::npos);

    // Журналы обоих режимов совпадают построчно: тихий режим меняет только консоль.
    CHECK(loud.state.day == days);
    // Кроме строки «Журнал партии: <каталог>» — каталоги у двух прогонов разные.
    auto protocol = [days](const fs::path& dir) {
        std::string out;
        for (const std::string& line : lines(rounds_text(dir, days, false))) {
            if (!line.starts_with("Журнал партии: ")) {
                out += line + '\n';
            }
        }
        return out;
    };
    CHECK(protocol(quiet.dir) == protocol(loud.dir));
    CHECK(read_file(quiet.dir / "summary.txt") == read_file(loud.dir / "summary.txt"));
    fs::remove_all(test_root());
}

int main() {
    std::cout << "Журналы партии\n";
    test::run("каталог, файлы кругов, итог, история запусков", &files_are_created);
    test::run("тайное — только в полном журнале, не в консоли", &secrets_only_in_full_log);
    test::run("тот же сид в ту же секунду — новый каталог", &same_seed_same_second);
    test::run("недостающие каталоги создаются", &nested_root_is_created);
    test::run("тихий режим: в консоли прогресс и итог, протокол — в журнале", &quiet_mode_console_and_files);
    return test::finish();
}
