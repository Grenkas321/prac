// Поддельный сервис модели для ручной проверки без ключа:
//
//   make fake-server                          # собрать build/make/fake_llm_server
//   ./build/make/fake_llm_server --port 8089  # терминал 1
//   ./build/make/mafia --ai 3 --ai-url http://127.0.0.1:8089/v1   # терминал 2
//
// Отвечает корректным JSON по заданию из запроса (тот же сервер, что в
// tests/test_ai.cpp). Ctrl+C — остановить.
#include "../tests/fake_llm_server.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <string_view>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

std::string line_after(const std::string& text, std::string_view marker) {
    const auto pos = text.find(marker);
    if (pos == std::string::npos) {
        return "?";
    }
    const auto from = pos + marker.size();
    return text.substr(from, text.find_first_of("\\\n", from) - from);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8089;
    if (argc == 3 && std::string_view(argv[1]) == "--port") {
        const std::string_view v = argv[2];
        const auto [end, err] = std::from_chars(v.data(), v.data() + v.size(), port);
        if (err != std::errc{} || end != v.data() + v.size() || port < 1 || port > 65535) {
            std::cerr << "порт — число от 1 до 65535\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "Использование: " << argv[0] << " [--port P]\n";
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        fake::Server server(port);
        std::cout << "Поддельная модель слушает " << server.url() << " (OpenAI: /chat/completions, Anthropic: /messages)\n"
                  << "Ctrl+C — остановить.\n"
                  << std::flush;
        std::size_t shown = 0;
        // Сам ключ не печатаем — только факт, что он пришёл в заголовке.
        auto show_new = [&] {
            const auto got = server.received();
            for (; shown < got.size(); ++shown) {
                const auto& r = got[shown];
                const bool has_key = r.headers.find("Authorization:") != std::string::npos ||
                                     r.headers.find("x-api-key:") != std::string::npos;
                std::cout << "#" << shown + 1 << " " << r.path << ": Игрок " << line_after(r.body, "Ты — Игрок ")
                          << " задание " << line_after(r.body, "ЗАДАНИЕ: ") << (has_key ? ", ключ передан" : ", без ключа")
                          << "\n" << std::flush;
            }
        };
        while (!g_stop) {
            show_new();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        show_new();  // запросы, пришедшие за последние 100 мс
        std::cout << "Остановлен, запросов принято: " << shown << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
