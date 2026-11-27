#include "ai/http_client.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mafia {

namespace {

// Писать в канал, который закрыл упавший curl, — сигнал SIGPIPE, по
// умолчанию убивающий процесс. Нам нужна ошибка записи (EPIPE), а не смерть партии.
void ignore_sigpipe() {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

struct Pipe {
    int fd[2] = {-1, -1};
    bool open() { return ::pipe(fd) == 0; }
    void close_read() { close_fd(0); }
    void close_write() { close_fd(1); }
    ~Pipe() {
        close_fd(0);
        close_fd(1);
    }

private:
    void close_fd(int i) {
        if (fd[i] >= 0) {
            ::close(fd[i]);
            fd[i] = -1;
        }
    }
};

}  // namespace

CurlClient::CurlClient() {
    ignore_sigpipe();
}

HttpResult CurlClient::post(const std::string& url, const std::vector<std::string>& headers, const std::string& body,
                            std::chrono::milliseconds timeout) {
    HttpResult result;
    // Только http(s): curl умеет и file://, и прочее — модели это не нужно.
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        result.error = "адрес должен начинаться с http:// или https://";
        return result;
    }

    Pipe in, out, hdr;
    if (!in.open() || !out.open() || !hdr.open()) {
        result.error = std::string("pipe: ") + std::strerror(errno);
        return result;
    }

    const std::string seconds = std::to_string(std::max<long long>(1, (timeout.count() + 999) / 1000));
    // В аргументах — ничего секретного: адрес, таймаут, служебные флаги.
    std::vector<std::string> args = {"curl",          "--silent",     "--show-error", "--max-time", seconds,
                                     "--request",     "POST",         "--header",     "@/dev/fd/3", "--data-binary",
                                     "@-",            "--write-out",  "\n%{http_code}", "--",      url};
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in.fd[0], 0);   // stdin  <- тело
    posix_spawn_file_actions_adddup2(&actions, out.fd[1], 1);  // stdout -> ответ
    posix_spawn_file_actions_adddup2(&actions, hdr.fd[0], 3);  // fd 3   <- заголовки
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    for (int fd : {in.fd[0], in.fd[1], out.fd[0], out.fd[1], hdr.fd[0], hdr.fd[1]}) {
        if (fd > 3) {
            posix_spawn_file_actions_addclose(&actions, fd);
        }
    }
    // Окружение curl — без ключа: ему он не нужен (ключ уже в заголовках).
    std::vector<char*> env;
    for (char** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, "MAFIA_AI_KEY=", 13) != 0) {
            env.push_back(*e);
        }
    }
    env.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, "curl", &actions, nullptr, argv.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        result.error = std::string("не удалось запустить curl: ") + std::strerror(spawned);
        return result;
    }
    in.close_read();
    out.close_write();
    hdr.close_read();

    std::string header_text;
    for (const std::string& h : headers) {
        header_text += h + "\n";
    }
    // Заголовки и тело небольшие (единицы килобайт) — пишем целиком, потом читаем ответ.
    const bool sent = write_all(hdr.fd[1], header_text) && (hdr.close_write(), true) && write_all(in.fd[1], body);
    in.close_write();
    hdr.close_write();

    std::string raw;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(out.fd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        if (raw.size() < kMaxResponse + 16) {
            raw.append(buf, static_cast<std::size_t>(n));
        }
    }
    int wstatus = 0;
    while (::waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        const int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
        result.error = code == 28 ? "превышено время ожидания"
                       : code == 7 ? "не удалось подключиться"
                                   : "curl завершился с кодом " + std::to_string(code);
        return result;
    }
    if (!sent) {
        result.error = "не удалось передать запрос curl";
        return result;
    }
    if (raw.size() > kMaxResponse) {
        result.error = "ответ больше 1 МБ";
        return result;
    }
    // Последняя строка — код ответа (--write-out), всё до неё — тело.
    const std::size_t nl = raw.rfind('\n');
    if (nl == std::string::npos) {
        result.error = "нет кода ответа";
        return result;
    }
    result.status = std::atoi(raw.c_str() + nl + 1);
    result.body = raw.substr(0, nl);
    return result;
}

}  // namespace mafia
