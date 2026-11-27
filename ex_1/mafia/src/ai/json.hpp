// Свой JSON: значение, разбор и экранирование строк — без внешних библиотек.
//
// Разбор строгий (RFC 8259): объекты, массивы, строки с \uXXXX (включая
// суррогатные пары), числа, true/false/null. Глубина вложенности ограничена:
// ответ сети — недоверенные данные, и «[[[[[...» не должен переполнить стек.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mafia::json {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> data;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    const bool* as_bool() const { return std::get_if<bool>(&data); }
    const double* as_number() const { return std::get_if<double>(&data); }
    const std::string* as_string() const { return std::get_if<std::string>(&data); }
    const Array* as_array() const {
        auto p = std::get_if<std::shared_ptr<Array>>(&data);
        return p ? p->get() : nullptr;
    }
    const Object* as_object() const {
        auto p = std::get_if<std::shared_ptr<Object>>(&data);
        return p ? p->get() : nullptr;
    }
    // Поле объекта или nullptr (если это не объект или поля нет).
    const Value* get(std::string_view key) const;
};

inline constexpr std::size_t kMaxDepth = 64;

Value parse(std::string_view text);

// Строка в кавычках с экранированием — для сборки запросов.
std::string quote(std::string_view text);

}  // namespace mafia::json
