// HTTP(S)-клиент для обращения к модели.
//
// Своего TLS мы не пишем: запрос выполняет системный curl, запущенный через
// posix_spawnp. Секреты в командную строку не попадают — иначе их видно
// любому пользователю машины в `ps`. Заголовки (с ключом) curl читает из
// канала, подключённого как дескриптор 3 (-H @/dev/fd/3), тело — из stdin.
//
// Любой сбой — не исключение, а HttpResult с error: вызывающий откатывается
// на эвристику, партия не падает.
#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace mafia {

struct HttpResult {
    int status = 0;         // код ответа HTTP; 0 — до HTTP не дошло
    std::string body;
    std::string error;      // пусто — запрос выполнен (status может быть и не 200)
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    // headers — строки «Имя: значение»; могут содержать секреты.
    virtual HttpResult post(const std::string& url, const std::vector<std::string>& headers,
                            const std::string& body, std::chrono::milliseconds timeout) = 0;
};

class CurlClient final : public HttpClient {
public:
    CurlClient();
    HttpResult post(const std::string& url, const std::vector<std::string>& headers, const std::string& body,
                    std::chrono::milliseconds timeout) override;

    static constexpr std::size_t kMaxResponse = 1 << 20;  // 1 МБ — больше от модели не ждём
};

}  // namespace mafia
