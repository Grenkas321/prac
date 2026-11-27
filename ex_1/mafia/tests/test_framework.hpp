// Мини-фреймворк для тестов, без внешних библиотек.
//
// Тестовый файл — отдельная программа:
//   int main() {
//       test::run("название случая", &функция_без_аргументов);
//       ...
//       return test::finish();
//   }
// Внутри случая проверки делаются макросом CHECK(условие).
// Код возврата 0 — все случаи прошли (так его понимают make test и ctest).
#pragma once

#include <exception>
#include <iostream>

namespace test {

inline int g_failed_checks = 0;  // провалы в текущем случае
inline int g_total_cases = 0;
inline int g_failed_cases = 0;

inline void report_failure(const char* expr, const char* file, int line) {
    ++g_failed_checks;
    std::cout << "\n      ПРОВАЛ: " << expr << "  (" << file << ':' << line << ')';
}

// Запустить один случай. Исключение, вылетевшее из случая, тоже считается провалом.
inline void run(const char* name, void (*body)()) {
    ++g_total_cases;
    g_failed_checks = 0;
    std::cout << "  " << name << "..." << std::flush;
    try {
        body();
    } catch (const std::exception& e) {
        ++g_failed_checks;
        std::cout << "\n      ИСКЛЮЧЕНИЕ: " << e.what();
    } catch (...) {
        ++g_failed_checks;
        std::cout << "\n      ИСКЛЮЧЕНИЕ неизвестного типа";
    }
    if (g_failed_checks == 0) {
        std::cout << " ок\n";
    } else {
        ++g_failed_cases;
        std::cout << "\n  -> случай провален\n";
    }
}

inline int finish() {
    std::cout << "Случаев: " << g_total_cases << ", провалено: " << g_failed_cases << '\n';
    return g_failed_cases == 0 ? 0 : 1;
}

}  // namespace test

// Проверка не прерывает случай: печатаем все провалы сразу, а не только первый.
#define CHECK(expr)                                              \
    do {                                                         \
        if (!(expr)) {                                           \
            ::test::report_failure(#expr, __FILE__, __LINE__);   \
        }                                                        \
    } while (false)
