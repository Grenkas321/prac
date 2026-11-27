// Свой разбор подмножества YAML — ровно того, что нужно конфигу партии.
//
// Поддерживается:
//   # комментарии (с начала строки или после пробела), пустые строки, «---»
//   ключ: значение                 — скаляр верхнего уровня
//   ключ: "строка" / 'строка'      — строки в кавычках
//   ключ: [a, b, "c d"]            — список в одну строку (flow)
//   ключ:                          — список блоком:
//     - a
//     - b
//
// Не поддерживается (и даёт понятную ошибку, а не тихо «как-нибудь»):
// вложенные словари, якоря и ссылки (&, *), теги (!), многострочные
// строки (| и >), табуляция в отступах, повтор ключа.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mafia {

class YamlError : public std::runtime_error {
public:
    YamlError(std::size_t line, std::size_t column, const std::string& message);
    std::size_t line() const noexcept { return line_; }
    std::size_t column() const noexcept { return column_; }

private:
    std::size_t line_;
    std::size_t column_;
};

using YamlScalar = std::string;
using YamlList = std::vector<std::string>;

struct YamlEntry {
    std::string key;
    std::variant<YamlScalar, YamlList> value;
    std::size_t line = 0;  // где ключ — для сообщений об ошибках выше по течению
};

// Словарь верхнего уровня в порядке появления ключей.
std::vector<YamlEntry> parse_yaml(std::string_view text);

}  // namespace mafia
