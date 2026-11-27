#include "log/game_log.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace mafia {

namespace {

// «2026-10-03_14-05-07» по местному времени. Только цифры, дефис и
// подчёркивание: имя каталога одинаково допустимо в Linux и Windows.
std::string timestamp(std::chrono::system_clock::time_point t) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(t);
    std::tm local{};
    localtime_r(&raw, &local);  // потокобезопасный вариант std::localtime (POSIX)
    std::ostringstream s;
    s << std::put_time(&local, "%Y-%m-%d_%H-%M-%S");
    return s.str();
}

std::ofstream open_or_throw(const fs::path& file, std::ios::openmode mode) {
    std::ofstream out(file, mode);
    if (!out) {
        throw std::runtime_error("не удалось открыть файл журнала: " + file.string());
    }
    return out;
}

}  // namespace

GameLog::GameLog(fs::path root, std::uint64_t seed, Level level, std::chrono::system_clock::time_point started)
    : root_(std::move(root)), level_(level) {
    fs::create_directories(root_);  // все недостающие уровни; если уже есть — ничего

    // Уникальный каталог: create_directory атомарно создаёт каталог и
    // возвращает false, если он уже существует. Два запуска с одним зерном
    // в одну секунду (даже из разных процессов) получат _2, _3, ...
    const std::string base = timestamp(started) + "_seed" + std::to_string(seed);
    for (int attempt = 1;; ++attempt) {
        fs::path candidate = root_ / (attempt == 1 ? base : base + "_" + std::to_string(attempt));
        if (fs::create_directory(candidate)) {
            dir_ = std::move(candidate);
            break;
        }
        if (attempt > 1000) {
            throw std::runtime_error("не удалось создать каталог партии в " + root_.string());
        }
    }
}

void GameLog::begin_round(int round) {
    std::ostringstream name;
    name << "round_" << std::setw(2) << std::setfill('0') << round << ".txt";
    round_.close();
    round_ = open_or_throw(dir_ / name.str(), std::ios::out | std::ios::trunc);
}

void GameLog::secret(std::string_view line) {
    if (level_ == Level::Full && round_.is_open()) {
        round_ << kSecretMark << line << '\n';
    }
}

void GameLog::write_summary(std::string_view text) {
    std::ofstream out = open_or_throw(dir_ / "summary.txt", std::ios::out | std::ios::trunc);
    out << text;
}

void GameLog::append_history(std::string_view line) {
    // app: каждая запись — в конец файла. Строки разных запусков не перемешаются,
    // пока каждая записывается одним коротким вызовом.
    std::ofstream out = open_or_throw(root_ / "history.txt", std::ios::out | std::ios::app);
    out << line << '\n';
}

// ---------------------------------------------------------------------------

TeeBuffer::int_type TeeBuffer::overflow(int_type ch) {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
        return traits_type::not_eof(ch);
    }
    const char c = traits_type::to_char_type(ch);
    if (first_ != nullptr) {
        first_->sputc(c);
    }
    if (log_ != nullptr) {
        log_->round_buffer()->sputc(c);
    }
    return ch;
}

std::streamsize TeeBuffer::xsputn(const char* s, std::streamsize n) {
    if (first_ != nullptr) {
        first_->sputn(s, n);
    }
    if (log_ != nullptr) {
        log_->round_buffer()->sputn(s, n);
    }
    return n;
}

int TeeBuffer::sync() {
    const int a = first_ != nullptr ? first_->pubsync() : 0;
    const int b = log_ != nullptr ? log_->round_buffer()->pubsync() : 0;
    return a == 0 && b == 0 ? 0 : -1;
}

}  // namespace mafia
