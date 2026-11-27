// GameLog — журнал партии в файлах (std::filesystem).
//
//   <root>/                              например logs/
//     history.txt                        общая история запусков: строка на партию
//     2026-10-03_14-05-07_seed2026/      каталог партии: дата, время, зерно
//       round_01.txt                     круг = сутки: утро, день и следующая ночь
//       round_02.txt
//       summary.txt                      итог и статистика по каждому игроку
//
// Краткий журнал (Level::Brief) содержит только публичное — то же, что видит
// консоль. Полный (Level::Full) — ещё и тайное, каждая строка с пометкой
// «[ТАЙНО]». В консоль тайное не попадает никогда: оно пишется только сюда.
//
// Журнал принадлежит ведущему (одна нить), поэтому без блокировок.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>

namespace mafia {

class GameLog {
public:
    enum class Level { Brief, Full };

    // Создаёт <root> (если нужно) и уникальный каталог партии внутри.
    // started — момент начала партии (по нему имя каталога).
    GameLog(std::filesystem::path root, std::uint64_t seed, Level level,
            std::chrono::system_clock::time_point started = std::chrono::system_clock::now());

    GameLog(const GameLog&) = delete;
    GameLog& operator=(const GameLog&) = delete;

    Level level() const noexcept { return level_; }
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::filesystem::path& dir() const noexcept { return dir_; }

    // Начать файл круга: round_NN.txt. Публичное и тайное пишутся в текущий круг.
    void begin_round(int round);

    // Куда писать публичный текст текущего круга (для тройника «консоль + файл»).
    std::streambuf* round_buffer() noexcept { return round_.rdbuf(); }

    // Тайная строка: только в полный журнал, с пометкой.
    void secret(std::string_view line);

    // Итоговый файл партии.
    void write_summary(std::string_view text);

    // Строка в общую историю запусков (дописывается в конец).
    void append_history(std::string_view line);

    static constexpr std::string_view kSecretMark = "[ТАЙНО] ";

private:
    std::filesystem::path root_;
    std::filesystem::path dir_;
    Level level_;
    std::ofstream round_;
};

// Поток-«тройник»: всё, что в него пишут, уходит сразу в два буфера.
// Ведущий пишет публичный текст один раз — он попадает и в консоль, и в журнал.
class TeeBuffer : public std::streambuf {
public:
    TeeBuffer(std::streambuf* first, GameLog* log) : first_(first), log_(log) {}

protected:
    int_type overflow(int_type ch) override;
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    int sync() override;

private:
    std::streambuf* first_;  // может быть nullptr — тогда только журнал (тихий режим)
    GameLog* log_;  // может быть nullptr — тогда только первый буфер
};

}  // namespace mafia
