// Локальный поддельный сервис языковой модели — для проверки ИИ-агента без
// настоящего ключа и без сети.
//
// Слушает 127.0.0.1 на свободном порту (его выбирает ядро), обслуживает
// соединения в своей нити и отвечает в формате настоящих API:
//   POST .../chat/completions -> {"choices":[{"message":{"content": "..."}}]}  (OpenAI)
//   POST .../messages         -> {"content":[{"type":"text","text": "..."}]}  (Anthropic)
//
// Поведение задаётся очередью сценариев: каждый следующий запрос получает
// следующий сценарий (код ответа, сырое тело, задержка, обрыв соединения,
// мусор вместо HTTP). Очередь пуста — «умный» ответ: сервер читает из
// запроса задание и допустимые цели и отвечает корректным JSON.
// Все принятые запросы (путь, заголовки, тело) записываются — тест смотрит,
// что именно ушло в сеть.
//
// Используется в tests/test_ai.cpp и в tools/fake_llm_server.cpp (ручной запуск).
#pragma once

#include "ai/json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fake {

// Что сделать с очередным запросом.
struct Script {
    enum class Kind {
        Smart,    // корректный ответ по заданию из запроса
        Reply,    // ответить: код status, тело body как есть
        Content,  // корректная обёртка формата, а текст модели — content как есть
        Close,    // закрыть соединение, ничего не ответив
        Garbage,  // прислать не-HTTP
    };
    Kind kind = Kind::Smart;
    int status = 200;
    std::string body;     // Reply: всё тело ответа; Content: текст модели
    int delay_ms = 0;     // пауза перед ответом (для проверки таймаута)

    static Script smart() { return {}; }
    static Script reply(int status, std::string body) { return {Kind::Reply, status, std::move(body), 0}; }
    static Script content(std::string text) { return {Kind::Content, 200, std::move(text), 0}; }
    static Script close() { return {Kind::Close, 0, {}, 0}; }
    static Script garbage() { return {Kind::Garbage, 0, {}, 0}; }
    Script& after(int ms) {
        delay_ms = ms;
        return *this;
    }
};

struct Received {
    std::string path;
    std::string headers;  // как пришли, строки через \r\n
    std::string body;
};

class Server {
public:
    explicit Server(int port = 0) {  // 0 — свободный порт выберет ядро
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("fake: socket");
        }
        const int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // только локально — наружу не слушаем
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        socklen_t len = sizeof addr;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(fd_, 64) != 0 || ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(fd_);
            throw std::runtime_error("fake: bind/listen");
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }

    ~Server() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int port() const noexcept { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }

    void push(Script s) {
        std::lock_guard lock(mutex_);
        scripts_.push_back(std::move(s));
    }
    std::vector<Received> received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }
    std::size_t count() const {
        std::lock_guard lock(mutex_);
        return received_.size();
    }

    // Ответ модели на задание из текста запроса (без обёртки формата).
    static std::string smart_answer(const std::string& prompt) {
        const std::string task = after(prompt, "ЗАДАНИЕ: ");
        const std::string targets = after(prompt, "Допустимые цели (номера): ");
        const std::string first = targets.substr(0, targets.find(','));
        if (task.starts_with("vote")) {
            return "{\"action\":\"vote\",\"target\":" + first +
                   ",\"reasoning\":\"первый из допустимых\",\"message\":\"Голосую против Игрока " + first + ".\"}";
        }
        if (task.starts_with("speech") && !first.empty()) {
            return "{\"action\":\"accuse\",\"target\":" + first +
                   ",\"reasoning\":\"проверка\",\"message\":\"Игрок " + first + " ведёт себя подозрительно.\"}";
        }
        return "{\"action\":\"say\",\"target\":null,\"reasoning\":\"прощание\",\"message\":\"Удачи городу!\"}";
    }

private:
    static std::string after(const std::string& text, const std::string& marker) {
        const auto pos = text.find(marker);
        if (pos == std::string::npos) {
            return {};
        }
        const auto from = pos + marker.size();
        return text.substr(from, text.find('\n', from) - from);
    }

    void loop() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) {
                continue;  // раз в 20 мс проверяем, не пора ли остановиться
            }
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client >= 0) {
                serve(client);
                ::close(client);
            }
        }
    }

    // Прочитать запрос целиком: заголовки до пустой строки, затем Content-Length байт.
    static bool read_request(int c, Received& r) {
        std::string data;
        char buf[4096];
        std::size_t need = std::string::npos;
        std::size_t head_end = std::string::npos;
        while (need == std::string::npos || data.size() < need) {
            pollfd p{c, POLLIN, 0};
            if (::poll(&p, 1, 5000) <= 0) {
                return false;
            }
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) {
                return false;
            }
            data.append(buf, static_cast<std::size_t>(n));
            if (head_end == std::string::npos && (head_end = data.find("\r\n\r\n")) != std::string::npos) {
                std::size_t length = 0;
                std::string lower = data.substr(0, head_end);
                for (char& ch : lower) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (const auto cl = lower.find("content-length:"); cl != std::string::npos) {
                    length = std::stoul(lower.substr(cl + 15));
                }
                need = head_end + 4 + length;
            }
        }
        const auto line_end = data.find("\r\n");
        const std::string line = data.substr(0, line_end);  // POST /v1/... HTTP/1.1
        const auto sp1 = line.find(' ');
        r.path = line.substr(sp1 + 1, line.find(' ', sp1 + 1) - sp1 - 1);
        r.headers = data.substr(line_end + 2, head_end - line_end - 2);
        r.body = data.substr(head_end + 4);
        return true;
    }

    // Текст пользовательского сообщения из тела запроса любого формата.
    static std::string prompt_of(const std::string& body) {
        try {
            const mafia::json::Value v = mafia::json::parse(body);
            const mafia::json::Value* msgs = v.get("messages");
            const mafia::json::Array* arr = msgs ? msgs->as_array() : nullptr;
            if (arr && !arr->empty()) {
                const mafia::json::Value* content = arr->back().get("content");
                if (content && content->as_string()) {
                    return *content->as_string();
                }
            }
        } catch (const mafia::json::ParseError&) {
        }
        return {};
    }

    static std::string wrap(bool anthropic, const std::string& text) {
        if (anthropic) {
            return "{\"id\":\"msg_fake\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\","
                   "\"text\":" + mafia::json::quote(text) + "}],\"stop_reason\":\"end_turn\"}";
        }
        return "{\"id\":\"chatcmpl-fake\",\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":"
               "{\"role\":\"assistant\",\"content\":" + mafia::json::quote(text) + "},\"finish_reason\":\"stop\"}]}";
    }

    static void send_all(int c, const std::string& data) {
        std::size_t done = 0;
        while (done < data.size()) {
            // MSG_NOSIGNAL: клиент мог уже уйти по таймауту — без SIGPIPE.
            const ssize_t n = ::send(c, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            if (n <= 0) {
                return;
            }
            done += static_cast<std::size_t>(n);
        }
    }

    void serve(int c) {
        Received r;
        if (!read_request(c, r)) {
            return;
        }
        Script s;
        {
            std::lock_guard lock(mutex_);
            received_.push_back(r);
            if (!scripts_.empty()) {
                s = std::move(scripts_.front());
                scripts_.pop_front();
            }
        }
        if (s.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(s.delay_ms));
        }
        const bool anthropic = r.path.ends_with("/messages");
        std::string body;
        int status = 200;
        switch (s.kind) {
            case Script::Kind::Close:
                return;
            case Script::Kind::Garbage:
                send_all(c, "это не HTTP\r\n\r\n");
                return;
            case Script::Kind::Reply:
                status = s.status;
                body = s.body;
                break;
            case Script::Kind::Content:
                body = wrap(anthropic, s.body);
                break;
            case Script::Kind::Smart:
                body = wrap(anthropic, smart_answer(prompt_of(r.body)));
                break;
        }
        send_all(c, "HTTP/1.1 " + std::to_string(status) + " Fake\r\nContent-Type: application/json\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::deque<Script> scripts_;
    std::vector<Received> received_;
};

}  // namespace fake
