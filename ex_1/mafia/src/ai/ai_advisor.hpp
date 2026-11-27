// ИИ-агент: советник игрока, который спрашивает языковую модель через API.
//
//   запрос ведущего (Request)
//     -> build_prompt: системная часть + сообщение — ТОЛЬКО из Request
//     -> бюджет обращений за круг (атомарно)
//     -> HttpClient::post (OpenAI /chat/completions или Anthropic /messages)
//     -> parse_reply: разбор и проверка ответа
//     -> AdvisorAnswer или «совета нет» (игрок решает эвристикой)
//
// Ответ модели — JSON {"action", "target", "reasoning", "message"}:
// действие, цель, обоснование, реплика. Любой сбой — откат на эвристику.
#pragma once

#include "ai/http_client.hpp"
#include "core/shared_ptr.hpp"
#include "roles/advisor.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace mafia {

enum class AiFormat { OpenAI, Anthropic };

struct AiConfig {
    std::string base_url = "https://vireonix.ai/v1";  // бесплатный OpenAI-совместимый, без ключа
    std::string model = "auto";
    AiFormat format = AiFormat::OpenAI;
    std::string api_key;  // только из переменной окружения MAFIA_AI_KEY; может быть пустым
    std::chrono::milliseconds timeout{20'000};
    int calls_per_round = 10;  // лимит обращений за круг на всех ИИ-игроков
    int max_tokens = 300;

    // Ключ — из окружения и только оттуда (в командную строку он не попадает).
    static std::string key_from_environment();
};

// Лимит обращений за круг. Счётчик и номер круга упакованы в ОДНО атомарное
// 64-битное слово: «новый круг -> сбросить счётчик» и «занять обращение»
// происходят одной операцией compare_exchange — без мьютекса и без окна,
// в котором две нити обе увидят «последнее свободное» обращение.
class AiBudget {
public:
    explicit AiBudget(int per_round) : per_round_(per_round) {}
    bool try_acquire(int round) noexcept;
    int used(int round) const noexcept;
    int per_round() const noexcept { return per_round_; }

private:
    const int per_round_;
    std::atomic<std::uint64_t> state_{0};  // старшие 32 бита — круг, младшие — занято
};

// Журнал диалога с моделью — отдельный файл. Пишут нити всех ИИ-игроков,
// поэтому под мьютексом. Ключ сюда не попадает: заголовки мы не пишем.
class AiDialogLog {
public:
    explicit AiDialogLog(const std::filesystem::path& file);
    void write(const std::string& text);
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::mutex mutex_;
    std::ofstream out_;
};

struct AiPrompt {
    std::string system;  // кто ты, правила, характер, формат ответа, ТВОИ тайные сведения
    std::string user;    // что видно всем: день, живые, история; задание и допустимые цели
};

// Собрать запрос к модели. Источник — только Request: снимок публичного
// состояния и личная сводка этого игрока. Чужих тайн там нет по построению.
AiPrompt build_prompt(const Request& request, std::string_view personality);

// Тело HTTP-запроса в нужном формате.
std::string request_body(const AiConfig& config, const AiPrompt& prompt);

struct ParsedReply {
    std::optional<AdvisorAnswer> answer;  // пусто — ответ отвергнут
    std::string error;                    // почему отвергнут
    std::string reasoning;                // обоснование модели — только в журнал
};

// Разобрать и проверить ответ сервиса (тело HTTP) для данного запроса.
ParsedReply parse_reply(const Request& request, AiFormat format, std::string_view body);

class AiAdvisor final : public Advisor {
public:
    AiAdvisor(SharedPtr<const AiConfig> config, SharedPtr<HttpClient> client, SharedPtr<AiBudget> budget,
              SharedPtr<AiDialogLog> log, std::string personality);

    std::optional<AdvisorAnswer> advise(const Request& request) override;

    const std::string& personality() const noexcept { return personality_; }

private:
    SharedPtr<const AiConfig> config_;
    SharedPtr<HttpClient> client_;
    SharedPtr<AiBudget> budget_;
    SharedPtr<AiDialogLog> log_;
    std::string personality_;
};

// Характеры ИИ-игроков (по образцу из задания) — выбираются по месту.
std::string_view personality_for(int index);

}  // namespace mafia
