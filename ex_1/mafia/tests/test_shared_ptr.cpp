// Тесты SharedPtr<T>: семантика владения, удалители, приведения, сравнения,
// хеш, число выделений памяти, безопасность исключений и многопоточность.
#include "core/shared_ptr.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <latch>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using mafia::SharedPtr;
using mafia::make_shared_ptr;

// ---------------------------------------------------------------------------
// Подмена глобального operator new: считаем выделения и умеем «отказать».
// ---------------------------------------------------------------------------
namespace alloc {
std::atomic<long> allocated{0};
std::atomic<long> freed{0};
std::atomic<bool> fail_next{false};  // следующее выделение бросит std::bad_alloc
std::atomic<std::uintptr_t> last_ptr{0};
std::atomic<std::size_t> last_size{0};

long live() { return allocated.load() - freed.load(); }
}  // namespace alloc

void* operator new(std::size_t size) {
    if (alloc::fail_next.exchange(false)) {
        throw std::bad_alloc();
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    alloc::allocated.fetch_add(1);
    alloc::last_ptr.store(reinterpret_cast<std::uintptr_t>(p));
    alloc::last_size.store(size);
    return p;
}

// noinline: иначе g++ встраивает delete в вызывающий код, видит там free() для
// памяти «из operator new» и выдаёт ложное -Wmismatched-new-delete
// (он не знает, что наш operator new сам берёт память у malloc).
[[gnu::noinline]] void operator delete(void* p) noexcept {
    if (p != nullptr) {
        alloc::freed.fetch_add(1);
        std::free(p);
    }
}

[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }

// ---------------------------------------------------------------------------
// Вспомогательные типы.
// ---------------------------------------------------------------------------

// Считает живые и уничтоженные экземпляры (атомарно — пригодится в потоках).
struct Tracked {
    static inline std::atomic<int> alive{0};
    static inline std::atomic<int> destroyed{0};
    static void reset_counters() {
        alive = 0;
        destroyed = 0;
    }

    explicit Tracked(int v = 0) : value(v) { ++alive; }
    ~Tracked() {
        --alive;
        ++destroyed;
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;

    int value;
};

// Иерархия БЕЗ виртуального деструктора: delete через PlainBase* был бы UB.
struct PlainBase {
    int base_value = 1;
};
struct PlainDerived : PlainBase {
    static inline int destroyed = 0;
    ~PlainDerived() { ++destroyed; }
};

// Полиморфная иерархия для dynamic_pointer_cast.
struct Animal {
    virtual ~Animal() = default;
    virtual int legs() const { return 0; }
};
struct Dog : Animal {
    int legs() const override { return 4; }
    int barks = 3;
};
struct Bird : Animal {
    int legs() const override { return 2; }
};

struct Thrower {
    Thrower() { throw std::runtime_error("конструктор Thrower бросил исключение"); }
};

struct Node {
    static inline int destroyed = 0;
    explicit Node(int i) : id(i) {}
    ~Node() { ++destroyed; }
    int id;
    SharedPtr<Node> next;
};

// Присваивания через функции: прямое «p = p» компилятор (верно) считает
// подозрительным и предупреждает, а нам нужно проверить именно этот случай.
template <class P>
void copy_assign(P& dst, const P& src) {
    dst = src;
}
template <class P>
void move_assign(P& dst, P& src) {
    dst = std::move(src);
}

// ---------------------------------------------------------------------------
// Проверки на этапе компиляции.
// ---------------------------------------------------------------------------

// Два указателя — ни счётчика, ни удалителя внутри самого SharedPtr.
static_assert(sizeof(SharedPtr<Tracked>) == 2 * sizeof(void*));
// explicit operator bool: в if работает, неявно в bool/int — нет.
static_assert(std::is_constructible_v<bool, SharedPtr<int>>);
static_assert(!std::is_convertible_v<SharedPtr<int>, bool>);
// Конструктор от сырого указателя explicit: SharedPtr<int> p = new int; — ошибка.
static_assert(!std::is_convertible_v<int*, SharedPtr<int>>);
// Наследник -> база можно, наоборот и между чужими типами — нельзя.
static_assert(std::is_convertible_v<SharedPtr<Dog>, SharedPtr<Animal>>);
static_assert(!std::is_convertible_v<SharedPtr<Animal>, SharedPtr<Dog>>);
static_assert(!std::is_convertible_v<SharedPtr<Dog>, SharedPtr<Bird>>);
// Копирование, перемещение, обмен не бросают исключений.
static_assert(std::is_nothrow_copy_constructible_v<SharedPtr<Tracked>>);
static_assert(std::is_nothrow_move_constructible_v<SharedPtr<Tracked>>);
static_assert(std::is_nothrow_copy_assignable_v<SharedPtr<Tracked>>);
static_assert(std::is_nothrow_move_assignable_v<SharedPtr<Tracked>>);
static_assert(std::is_nothrow_swappable_v<SharedPtr<Tracked>>);

// ---------------------------------------------------------------------------
// Случаи.
// ---------------------------------------------------------------------------

void empty_pointers() {
    SharedPtr<Tracked> a;
    SharedPtr<Tracked> b(nullptr);
    SharedPtr<Tracked> c(static_cast<Tracked*>(nullptr));
    CHECK(a.get() == nullptr && b.get() == nullptr && c.get() == nullptr);
    CHECK(!a && !b && !c);
    CHECK(a.use_count() == 0 && b.use_count() == 0 && c.use_count() == 0);
}

void raw_pointer_and_access() {
    Tracked::reset_counters();
    {
        SharedPtr<Tracked> p(new Tracked(42));
        CHECK(p);
        CHECK(p.use_count() == 1);
        CHECK(p->value == 42);
        CHECK((*p).value == 42);
        CHECK(&*p == p.get());
        p->value = 7;  // доступ на запись через operator->
        CHECK(p.get()->value == 7);

        const SharedPtr<Tracked>& cp = p;
        cp->value = 8;  // константность поверхностная, как у Tracked* const
        CHECK(p->value == 8);
        CHECK(Tracked::alive == 1);
    }
    CHECK(Tracked::alive == 0);
    CHECK(Tracked::destroyed == 1);
}

void copy_increments_destroy_decrements() {
    Tracked::reset_counters();
    {
        SharedPtr<Tracked> a(new Tracked(1));
        {
            SharedPtr<Tracked> b(a);
            CHECK(a.use_count() == 2 && b.use_count() == 2);
            CHECK(a.get() == b.get());
            {
                SharedPtr<Tracked> c = b;
                CHECK(a.use_count() == 3);
            }
            CHECK(a.use_count() == 2);
        }
        CHECK(a.use_count() == 1);
        CHECK(Tracked::destroyed == 0);  // жив, пока есть хоть один владелец
    }
    CHECK(Tracked::destroyed == 1);  // ровно один раз
}

void move_does_not_touch_counter() {
    Tracked::reset_counters();
    SharedPtr<Tracked> a(new Tracked(5));
    Tracked* raw = a.get();
    SharedPtr<Tracked> b(std::move(a));
    CHECK(!a && a.use_count() == 0);  // источник опустел
    CHECK(b.get() == raw && b.use_count() == 1);

    SharedPtr<Tracked> c;
    c = std::move(b);
    CHECK(!b);
    CHECK(c.get() == raw && c.use_count() == 1);
    CHECK(Tracked::destroyed == 0);
}

void copy_assignment_releases_old() {
    Tracked::reset_counters();
    SharedPtr<Tracked> a(new Tracked(1));
    SharedPtr<Tracked> b(new Tracked(2));
    SharedPtr<Tracked> keep_b = b;
    CHECK(b.use_count() == 2);

    a = b;  // объект 1 потерял последнего владельца, у объекта 2 стало трое
    CHECK(Tracked::destroyed == 1);
    CHECK(a->value == 2);
    CHECK(b.use_count() == 3);

    a = nullptr;
    CHECK(!a && b.use_count() == 2);
}

void self_assignment() {
    Tracked::reset_counters();
    SharedPtr<Tracked> p(new Tracked(9));
    copy_assign(p, p);
    CHECK(p && p->value == 9 && p.use_count() == 1);
    move_assign(p, p);
    CHECK(p && p->value == 9 && p.use_count() == 1);

    SharedPtr<Tracked> q = p;  // второй указатель на тот же объект
    copy_assign(p, q);
    CHECK(p.use_count() == 2 && p.get() == q.get());
    CHECK(Tracked::destroyed == 0);
}

void assign_from_member_of_own_object() {
    Node::destroyed = 0;
    SharedPtr<Node> head = make_shared_ptr<Node>(1);
    head->next = make_shared_ptr<Node>(2);
    head->next->next = make_shared_ptr<Node>(3);

    // head — единственный владелец узла 1, а источник присваивания живёт ВНУТРИ узла 1.
    // Освободи мы сначала старое значение — узел 1 погиб бы вместе со своим next,
    // и копировать стало бы не из чего.
    head = head->next;
    CHECK(Node::destroyed == 1);
    CHECK(head->id == 2 && head.use_count() == 1);

    head = std::move(head->next);  // то же для перемещения
    CHECK(Node::destroyed == 2);
    CHECK(head->id == 3 && head.use_count() == 1);
}

void reset_variants() {
    Tracked::reset_counters();
    SharedPtr<Tracked> p(new Tracked(1));
    SharedPtr<Tracked> other = p;

    p.reset();  // объект жив: остался other
    CHECK(!p && p.use_count() == 0);
    CHECK(other.use_count() == 1 && Tracked::destroyed == 0);

    other.reset(new Tracked(2));  // старый объект теряет последнего владельца
    CHECK(Tracked::destroyed == 1);
    CHECK(other->value == 2 && other.use_count() == 1);

    int deleter_calls = 0;
    other.reset(new Tracked(3), [&deleter_calls](Tracked* t) {
        ++deleter_calls;
        delete t;
    });
    CHECK(Tracked::destroyed == 2 && other->value == 3);
    other.reset();
    CHECK(deleter_calls == 1 && Tracked::destroyed == 3);
}

void swap_variants() {
    SharedPtr<Tracked> a(new Tracked(1));
    SharedPtr<Tracked> b(new Tracked(2));
    SharedPtr<Tracked> b2 = b;
    a.swap(b);
    CHECK(a->value == 2 && b->value == 1);
    CHECK(a.use_count() == 2 && b.use_count() == 1);  // счётчики едут вместе с объектами
    swap(a, b);  // свободная функция (находится поиском по аргументам)
    CHECK(a->value == 1 && b->value == 2);
}

void comparisons() {
    SharedPtr<Tracked> a(new Tracked(1));
    SharedPtr<Tracked> b(new Tracked(2));
    SharedPtr<Tracked> a2 = a;
    SharedPtr<Tracked> empty;

    CHECK(a == a2);
    CHECK(a != b);
    CHECK((a < b) != (b < a));  // строгий порядок: ровно одно из двух
    CHECK((a < b) == std::less<Tracked*>{}(a.get(), b.get()));
    CHECK(a <= a2 && a >= a2);
    CHECK((a <=> a2) == std::strong_ordering::equal);

    CHECK(empty == nullptr && nullptr == empty);
    CHECK(a != nullptr && nullptr != a);

    // Разные типы: база и наследник смотрят на один объект — указатели равны.
    SharedPtr<Dog> dog = make_shared_ptr<Dog>();
    SharedPtr<Animal> animal = dog;
    CHECK(animal == dog);
}

void custom_deleters() {
    Tracked::reset_counters();

    // Лямбда с состоянием: вызывается один раз — когда уходит последний владелец.
    int calls = 0;
    {
        SharedPtr<Tracked> p(new Tracked(1), [&calls](Tracked* t) {
            ++calls;
            delete t;
        });
        SharedPtr<Tracked> q = p;
        p.reset();
        CHECK(calls == 0);
    }
    CHECK(calls == 1 && Tracked::destroyed == 1);

    // Массив: обычный delete здесь был бы UB, нужен delete[].
    {
        SharedPtr<int> arr(new int[4]{1, 2, 3, 4}, [](int* a) { delete[] a; });
        CHECK(arr.get()[3] == 4);
    }

    // Обычная функция в роли удалителя.
    static int function_calls = 0;
    struct Local {
        static void destroy(Tracked* t) {
            ++function_calls;
            delete t;
        }
    };
    { SharedPtr<Tracked> p(new Tracked(2), &Local::destroy); }
    CHECK(function_calls == 1 && Tracked::destroyed == 2);

    // «Невладеющий» указатель на объект со стека: удалитель ничего не делает.
    {
        Tracked on_stack(3);
        { SharedPtr<Tracked> view(&on_stack, [](Tracked*) {}); }
        CHECK(on_stack.value == 3 && Tracked::alive == 1);
    }

    // Нулевой указатель с удалителем: блока нет, удалитель не вызывается.
    int null_calls = 0;
    { SharedPtr<Tracked> p(static_cast<Tracked*>(nullptr), [&null_calls](Tracked*) { ++null_calls; }); }
    CHECK(null_calls == 0);
}

void type_erased_deletion() {
    // Невиртуальный деструктор у базы, но удаляется всё равно PlainDerived:
    // удалитель запомнил «родной» тип в момент создания.
    PlainDerived::destroyed = 0;
    {
        SharedPtr<PlainBase> p(new PlainDerived);
        CHECK(p->base_value == 1);
    }
    CHECK(PlainDerived::destroyed == 1);

    // То же при переходе к базе уже после создания.
    {
        SharedPtr<PlainDerived> d(new PlainDerived);
        SharedPtr<PlainBase> b = d;
        d.reset();
    }
    CHECK(PlainDerived::destroyed == 2);
}

void converting_copy_and_move() {
    SharedPtr<Dog> dog = make_shared_ptr<Dog>();
    SharedPtr<Animal> a1 = dog;  // копия: общий счётчик
    CHECK(dog.use_count() == 2 && a1->legs() == 4);

    SharedPtr<Animal> a2 = std::move(dog);  // перемещение: счётчик тот же
    CHECK(!dog && a2.use_count() == 2);

    SharedPtr<Animal> a3;
    a3 = make_shared_ptr<Bird>();  // перемещающее присваивание наследника
    CHECK(a3->legs() == 2 && a3.use_count() == 1);
}

void pointer_casts() {
    SharedPtr<Animal> animal = make_shared_ptr<Dog>();

    SharedPtr<Dog> dog = mafia::dynamic_pointer_cast<Dog>(animal);
    CHECK(dog && dog->barks == 3);
    CHECK(animal.use_count() == 2);  // приведение — ещё один владелец того же блока

    SharedPtr<Bird> bird = mafia::dynamic_pointer_cast<Bird>(animal);
    CHECK(!bird && bird.use_count() == 0);  // не Bird — пусто
    CHECK(animal.use_count() == 2);          // и счётчик не тронут

    SharedPtr<Dog> dog2 = mafia::static_pointer_cast<Dog>(animal);
    CHECK(dog2.get() == dog.get() && animal.use_count() == 3);

    SharedPtr<Animal> back = mafia::static_pointer_cast<Animal>(dog2);
    CHECK(back == animal && animal.use_count() == 4);

    // Объект живёт, пока жив хоть кто-то из приведённых.
    animal.reset();
    back.reset();
    dog2.reset();
    CHECK(dog.use_count() == 1 && dog->legs() == 4);

    SharedPtr<Animal> none;
    CHECK(!mafia::dynamic_pointer_cast<Dog>(none));
    CHECK(!mafia::static_pointer_cast<Dog>(none));
}

void hashing() {
    SharedPtr<Tracked> a(new Tracked(1));
    SharedPtr<Tracked> a2 = a;
    SharedPtr<Tracked> b(new Tracked(2));
    std::hash<SharedPtr<Tracked>> h;
    CHECK(h(a) == h(a2));
    CHECK(h(a) == std::hash<Tracked*>{}(a.get()));

    std::unordered_set<SharedPtr<Tracked>> set;
    set.insert(a);
    set.insert(a2);  // тот же объект — не добавится
    set.insert(b);
    CHECK(set.size() == 2);
    CHECK(set.count(a2) == 1);
    CHECK(a.use_count() == 3);  // множество хранит копию — тоже владелец
}

void allocation_count_and_layout() {
    // Сырой указатель: объект (new Tracked) + управляющий блок — два выделения.
    long before = alloc::allocated.load();
    SharedPtr<Tracked> raw(new Tracked(1));
    CHECK(alloc::allocated.load() - before == 2);

    // make_shared_ptr: одно выделение, объект лежит внутри управляющего блока.
    before = alloc::allocated.load();
    SharedPtr<Tracked> made = make_shared_ptr<Tracked>(2);
    CHECK(alloc::allocated.load() - before == 1);

    std::uintptr_t block_begin = alloc::last_ptr.load();
    std::uintptr_t block_end = block_begin + alloc::last_size.load();
    std::uintptr_t object = reinterpret_cast<std::uintptr_t>(made.get());
    CHECK(object > block_begin);  // в начале блока — счётчик, объект после него
    CHECK(object + sizeof(Tracked) <= block_end);
    CHECK(made->value == 2);

    // После уничтожения всё выделенное возвращено.
    long live_before = alloc::live();
    raw.reset();
    made.reset();
    CHECK(alloc::live() == live_before - 3);
}

void exception_safety() {
    Tracked::reset_counters();

    // Конструктор объекта бросает внутри make_shared_ptr: память блока возвращается.
    long live_before = alloc::live();
    bool thrown = false;
    try {
        SharedPtr<Thrower> p = make_shared_ptr<Thrower>();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(alloc::live() == live_before);

    // Не хватило памяти на управляющий блок: объект, отданный во владение, удаляется.
    Tracked* raw = new Tracked(1);
    thrown = false;
    alloc::fail_next = true;
    try {
        SharedPtr<Tracked> p(raw);
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(Tracked::destroyed == 1 && Tracked::alive == 0);
}

void threads_copy_and_destroy_one_pointer() {
    Tracked::reset_counters();
    constexpr int kThreads = 8;
    constexpr int kIterations = 200'000;

    SharedPtr<Tracked> shared = make_shared_ptr<Tracked>(7);
    std::atomic<bool> bad_value{false};
    std::atomic<long> max_seen{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                SharedPtr<Tracked> copy = shared;  // +1
                SharedPtr<Tracked> copy2(copy);    // +1
                if (copy2->value != 7) {
                    bad_value = true;
                }
                long seen = copy2.use_count();
                long prev = max_seen.load();
                while (seen > prev && !max_seen.compare_exchange_weak(prev, seen)) {
                }
            }  // −1, −1
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    CHECK(!bad_value);
    CHECK(max_seen.load() >= 3);         // копии действительно были
    CHECK(shared.use_count() == 1);      // вернулись к исходному
    CHECK(Tracked::destroyed == 0);      // ни одного преждевременного удаления
    shared.reset();
    CHECK(Tracked::destroyed == 1);      // ровно одно удаление
}

// Объект, в который пишут несколько потоков, а читает только деструктор.
struct Slots {
    static constexpr int kThreads = 8;
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<long> last_sum{0};
    long slot[kThreads] = {};  // НЕ atomic: видимость обеспечивает acq_rel в release()
    ~Slots() {
        long sum = 0;
        for (long v : slot) {
            sum += v;
        }
        last_sum = sum;
        ++destroyed;
    }
};

void threads_race_for_last_release() {
    constexpr int kRounds = 300;
    constexpr int kWrites = 1000;
    Slots::destroyed = 0;
    int wrong_sums = 0;
    int wrong_destroys = 0;

    for (int round = 0; round < kRounds; ++round) {
        SharedPtr<Slots> p = make_shared_ptr<Slots>();
        std::latch start(Slots::kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < Slots::kThreads; ++t) {
            // Копия захватывается здесь, в главном потоке; дальше у потока своя.
            threads.emplace_back([copy = p, t, &start]() mutable {
                start.arrive_and_wait();  // стартуем все одновременно
                for (int i = 0; i < kWrites; ++i) {
                    SharedPtr<Slots> tmp = copy;
                    ++tmp->slot[t];
                }
                copy.reset();  // кто-то из потоков окажется последним
            });
        }
        p.reset();  // главный поток отпускает свою копию сразу
        for (auto& th : threads) {
            th.join();
        }
        if (Slots::destroyed != round + 1) {
            ++wrong_destroys;
        }
        if (Slots::last_sum != long{Slots::kThreads} * kWrites) {
            ++wrong_sums;
        }
    }
    CHECK(wrong_destroys == 0);  // в каждом раунде объект удалён ровно один раз
    CHECK(wrong_sums == 0);      // деструктор увидел все записи всех потоков
}

int main() {
    std::cout << "SharedPtr<T>\n";
    test::run("пустые указатели (по умолчанию, nullptr)", &empty_pointers);
    test::run("сырой указатель, *, ->, get, bool", &raw_pointer_and_access);
    test::run("копия: +1, уничтожение: -1, удаление один раз", &copy_increments_destroy_decrements);
    test::run("перемещение не трогает счётчик", &move_does_not_touch_counter);
    test::run("копирующее присваивание отпускает старый объект", &copy_assignment_releases_old);
    test::run("самоприсваивание", &self_assignment);
    test::run("присваивание из поля собственного объекта", &assign_from_member_of_own_object);
    test::run("reset() / reset(p) / reset(p, d)", &reset_variants);
    test::run("swap (метод и свободная функция)", &swap_variants);
    test::run("сравнения ==, !=, <, <=>, с nullptr", &comparisons);
    test::run("пользовательские удалители", &custom_deleters);
    test::run("удаление через «родной» тип (стирание типа)", &type_erased_deletion);
    test::run("преобразование наследник -> база", &converting_copy_and_move);
    test::run("static_pointer_cast / dynamic_pointer_cast", &pointer_casts);
    test::run("std::hash и unordered_set", &hashing);
    test::run("число выделений памяти и размещение объекта", &allocation_count_and_layout);
    test::run("безопасность при исключениях", &exception_safety);
    test::run("потоки: параллельные копии одного указателя", &threads_copy_and_destroy_one_pointer);
    test::run("потоки: гонка за последнее освобождение", &threads_race_for_last_release);
    return test::finish();
}
