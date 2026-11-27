// SharedPtr<T> — собственный аналог std::shared_ptr с потокобезопасным счётчиком ссылок.
//
// Устройство:
//   SharedPtr<T>  = { T* ptr_;  ControlBlock* block_; }      — два указателя
//   ControlBlock  = { atomic счётчик владельцев; виртуальный destroy_object() }
//     PointerBlock<U, D> — объект выделен отдельно, хранит U* и удалитель D;
//     InplaceBlock<T>    — объект лежит прямо внутри блока (make_shared_ptr).
//
// Сознательно НЕ поддерживается: weak-указатели, массивы T[], SharedPtr<void>.
// Заголовок <memory> не подключается: всё нужное (размещающий new,
// явный вызов деструктора, удалитель по умолчанию) написано здесь.
#pragma once

#include <atomic>
#include <compare>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace mafia {

namespace detail {

// Управляющий блок: один на объект, общий для всех SharedPtr, которые им владеют.
// Тип объекта и тип удалителя здесь стёрты — о них знают только наследники.
class ControlBlock {
public:
    ControlBlock() noexcept = default;
    ControlBlock(const ControlBlock&) = delete;
    ControlBlock& operator=(const ControlBlock&) = delete;

    // Новый владелец появляется только копированием уже существующего, поэтому
    // счётчик сейчас >= 1 и не может обнулиться — упорядочивать нечего: relaxed.
    void add_ref() noexcept {
        owners_.fetch_add(1, std::memory_order_relaxed);
    }

    // acq_rel:
    //   release — все наши записи в объект «публикуются» вместе с уменьшением;
    //   acquire — тот, кто обнулил счётчик, видит записи всех прежних владельцев
    //             и только после этого запускает деструктор.
    void release() noexcept {
        if (owners_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy_object();  // шаг 1: деструктор объекта (через удалитель)
            delete this;       // шаг 2: память самого блока вместе с удалителем
        }
    }

    // Только для информации: к моменту возврата значение может устареть.
    long use_count() const noexcept {
        return owners_.load(std::memory_order_relaxed);
    }

protected:
    // Защищённый: удалять блок может только он сам (delete this в release).
    virtual ~ControlBlock() = default;

private:
    virtual void destroy_object() noexcept = 0;

    std::atomic<long> owners_{1};  // блок рождается вместе с первым владельцем
};

// Удалитель по умолчанию. Удаляет через U* — «родной» тип объекта,
// а не через T* того SharedPtr, который его держит.
template <class U>
struct DefaultDelete {
    void operator()(U* p) const noexcept { delete p; }
};

// Блок для объекта, выделенного отдельно (конструктор от сырого указателя).
template <class U, class D>
class PointerBlock final : public ControlBlock {
public:
    PointerBlock(U* p, D d) noexcept(std::is_nothrow_move_constructible_v<D>)
        : ptr_(p), deleter_(std::move(d)) {}

private:
    void destroy_object() noexcept override { deleter_(ptr_); }

    U* ptr_;
    [[no_unique_address]] D deleter_;  // пустой удалитель не занимает места
};

// Блок для make_shared_ptr: объект T лежит прямо в блоке — одно выделение памяти.
template <class T>
class InplaceBlock final : public ControlBlock {
public:
    template <class... Args>
    explicit InplaceBlock(Args&&... args) {
        ::new (static_cast<void*>(&value_)) T(std::forward<Args>(args)...);
    }

    // Пустой: value_ уже уничтожен в destroy_object(), повторно звать ~T() нельзя.
    ~InplaceBlock() override {}

    T* object() noexcept { return &value_; }

private:
    void destroy_object() noexcept override { value_.~T(); }

    // Объединение отключает автоматические конструктор и деструктор value_:
    // временем жизни объекта управляем вручную, независимо от памяти блока.
    union {
        T value_;
    };
};

}  // namespace detail

template <class T>
class SharedPtr;

template <class T, class... Args>
SharedPtr<T> make_shared_ptr(Args&&... args);

template <class To, class From>
SharedPtr<To> static_pointer_cast(const SharedPtr<From>& p) noexcept;

template <class To, class From>
SharedPtr<To> dynamic_pointer_cast(const SharedPtr<From>& p) noexcept;

template <class T>
class SharedPtr {
public:
    using element_type = T;

    // --- Пустые указатели: блока нет, use_count() == 0. ---
    constexpr SharedPtr() noexcept = default;
    constexpr SharedPtr(std::nullptr_t) noexcept {}

    // --- Захват владения сырым указателем. ---
    // U может быть наследником T: удалять будем именно как U.
    template <class U>
        requires std::is_convertible_v<U*, T*>
    explicit SharedPtr(U* p) : SharedPtr(p, detail::DefaultDelete<U>{}) {}

    // С пользовательским удалителем. Нулевой p даёт пустой указатель,
    // удалитель тогда не вызывается никогда.
    template <class U, class D>
        requires std::is_convertible_v<U*, T*> && std::is_invocable_v<D&, U*>
    SharedPtr(U* p, D d) {
        if (p == nullptr) {
            return;
        }
        try {
            block_ = new detail::PointerBlock<U, D>(p, std::move(d));
        } catch (...) {
            d(p);  // блок не создан — владеть объектом некому, удаляем сразу
            throw;
        }
        ptr_ = p;
    }

    // --- Копирование: ещё один владелец, счётчик +1. ---
    SharedPtr(const SharedPtr& other) noexcept : ptr_(other.ptr_), block_(other.block_) {
        if (block_ != nullptr) {
            block_->add_ref();
        }
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    SharedPtr(const SharedPtr<U>& other) noexcept : ptr_(other.ptr_), block_(other.block_) {
        if (block_ != nullptr) {
            block_->add_ref();
        }
    }

    // --- Перемещение: владелец тот же, сменилась лишь переменная — счётчик не трогаем. ---
    SharedPtr(SharedPtr&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)), block_(std::exchange(other.block_, nullptr)) {}

    template <class U>
        requires std::is_convertible_v<U*, T*>
    SharedPtr(SharedPtr<U>&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)), block_(std::exchange(other.block_, nullptr)) {}

    // --- Уничтожение: владельцем меньше; последний удаляет объект и блок. ---
    ~SharedPtr() {
        if (block_ != nullptr) {
            block_->release();
        }
    }

    // --- Присваивания: «скопировать во временный, обменяться, временный умрёт». ---
    // Сначала +1 у нового, потом −1 у старого — безопасно при a = a и при
    // a = a->next (когда старый объект сам владеет источником присваивания).
    SharedPtr& operator=(const SharedPtr& other) noexcept {
        SharedPtr(other).swap(*this);
        return *this;
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    SharedPtr& operator=(const SharedPtr<U>& other) noexcept {
        SharedPtr(other).swap(*this);
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& other) noexcept {
        SharedPtr(std::move(other)).swap(*this);
        return *this;
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    SharedPtr& operator=(SharedPtr<U>&& other) noexcept {
        SharedPtr(std::move(other)).swap(*this);
        return *this;
    }

    SharedPtr& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    // --- reset: те же присваивания, только из нового временного. ---
    void reset() noexcept { SharedPtr().swap(*this); }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    void reset(U* p) {
        SharedPtr(p).swap(*this);
    }

    template <class U, class D>
        requires std::is_convertible_v<U*, T*> && std::is_invocable_v<D&, U*>
    void reset(U* p, D d) {
        SharedPtr(p, std::move(d)).swap(*this);
    }

    void swap(SharedPtr& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(block_, other.block_);
    }

    friend void swap(SharedPtr& a, SharedPtr& b) noexcept { a.swap(b); }

    // --- Доступ. Константность «поверхностная», как у сырого указателя:
    // const SharedPtr<T> не делает объект константным. ---
    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }

    // explicit: if (p) работает, а int n = p; или p + 1 — нет.
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    long use_count() const noexcept { return block_ != nullptr ? block_->use_count() : 0; }

private:
    template <class U>
    friend class SharedPtr;

    template <class U, class... Args>
    friend SharedPtr<U> make_shared_ptr(Args&&... args);

    template <class To, class From>
    friend SharedPtr<To> static_pointer_cast(const SharedPtr<From>& p) noexcept;

    template <class To, class From>
    friend SharedPtr<To> dynamic_pointer_cast(const SharedPtr<From>& p) noexcept;

    // «Псевдоним» для приведений: блок общий с other, но смотрим через p.
    template <class U>
    SharedPtr(const SharedPtr<U>& other, T* p) noexcept : ptr_(p), block_(other.block_) {
        if (block_ != nullptr) {
            block_->add_ref();
        }
    }

    // Для make_shared_ptr: блок только что создан, его счётчик уже равен 1.
    explicit SharedPtr(detail::InplaceBlock<T>* block) noexcept
        : ptr_(block->object()), block_(block) {}

    T* ptr_ = nullptr;                      // на что смотрим (get, *, ->)
    detail::ControlBlock* block_ = nullptr;  // кем владеем (счётчик и удаление)
};

// Создать объект и управляющий блок одним выделением памяти.
template <class T, class... Args>
SharedPtr<T> make_shared_ptr(Args&&... args) {
    return SharedPtr<T>(new detail::InplaceBlock<T>(std::forward<Args>(args)...));
}

// Приведения: результат делит управляющий блок с исходным указателем.
template <class To, class From>
SharedPtr<To> static_pointer_cast(const SharedPtr<From>& p) noexcept {
    return SharedPtr<To>(p, static_cast<To*>(p.get()));
}

// Неудачный dynamic_cast даёт пустой SharedPtr; счётчик исходного не меняется.
template <class To, class From>
SharedPtr<To> dynamic_pointer_cast(const SharedPtr<From>& p) noexcept {
    if (To* q = dynamic_cast<To*>(p.get())) {
        return SharedPtr<To>(p, q);
    }
    return SharedPtr<To>();
}

// Сравнения — по адресу объекта (get()), как у сырых указателей.
// != , >, <=, >= компилятор C++20 выводит из == и <=> сам.
template <class T, class U>
bool operator==(const SharedPtr<T>& a, const SharedPtr<U>& b) noexcept {
    return a.get() == b.get();
}

template <class T, class U>
std::strong_ordering operator<=>(const SharedPtr<T>& a, const SharedPtr<U>& b) noexcept {
    return std::compare_three_way{}(a.get(), b.get());
}

template <class T>
bool operator==(const SharedPtr<T>& a, std::nullptr_t) noexcept {
    return !a;
}

template <class T>
std::strong_ordering operator<=>(const SharedPtr<T>& a, std::nullptr_t) noexcept {
    return std::compare_three_way{}(a.get(), static_cast<T*>(nullptr));
}

}  // namespace mafia

// Хеш — по адресу объекта: равные SharedPtr дают равные хеши
// (можно класть в std::unordered_set / ключом в std::unordered_map).
namespace std {
template <class T>
struct hash<mafia::SharedPtr<T>> {
    size_t operator()(const mafia::SharedPtr<T>& p) const noexcept { return hash<T*>{}(p.get()); }
};
}  // namespace std
