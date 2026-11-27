# Makefile проекта «Мафия» — основной способ сборки.
#
# Цели:
#   make            — то же, что make build
#   make build      — собрать build/make/mafia
#   make run        — собрать и запустить (аргументы: make run ARGS="--players 7")
#   make test       — собрать и прогнать все tests/test_*.cpp
#                     (V=1 — подробный вывод тестов и команды компилятора)
#   make tsan       — те же тесты под ThreadSanitizer (каталог build/tsan)
#   make mass       — только массовый тест с подробным выводом (MAFIA_MASS=множитель)
#   make strict     — сборка всего с -Werror (каталог build/strict)
#   make check      — всё для сдачи: strict + test + tsan
#   make fake-server — поддельный сервис модели для ручной проверки ИИ (без ключа)
#   make clean      — удалить каталог build/ целиком

CXX      := g++
# Команды компилятора видны только при V=1; иначе — короткая строка на файл.
Q        := $(if $(V),,@)
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread
# -Isrc: заголовки подключаются от корня src (#include "core/shared_ptr.hpp").
# -MMD -MP: компилятор сам пишет .d-файлы с зависимостями от заголовков.
CPPFLAGS := -Isrc -MMD -MP

BUILD_DIR := build/make
OBJ_DIR   := $(BUILD_DIR)/obj
TEST_DIR  := $(BUILD_DIR)/tests
TARGET    := $(BUILD_DIR)/mafia

# Все .cpp из src/ находятся автоматически — Makefile не надо править при добавлении файла.
SRCS     := $(shell find src -name '*.cpp')
OBJS     := $(SRCS:%.cpp=$(OBJ_DIR)/%.o)
# Тесты линкуются со всем кодом проекта, кроме main.o (у теста свой main).
MAIN_OBJ := $(OBJ_DIR)/src/main.o
LIB_OBJS := $(filter-out $(MAIN_OBJ),$(OBJS))

TEST_SRCS := $(sort $(wildcard tests/test_*.cpp))
TEST_OBJS := $(TEST_SRCS:%.cpp=$(OBJ_DIR)/%.o)
TEST_BINS := $(TEST_SRCS:tests/%.cpp=$(TEST_DIR)/%)

ARGS ?=
# Чем запускать тестовые бинарники (make tsan подставляет сюда обёртку).
RUN ?=

# ThreadSanitizer: -O1 -g — читаемые отчёты; halt_on_error=1 — первая же гонка
# роняет тест (код 66), и make tsan падает. setarch -R отключает рандомизацию
# адресов: на новых ядрах Linux без этого TSan может не стартовать.
TSAN_CXXFLAGS := -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=thread

.PHONY: all build run test tsan clean fake-server mass strict check all-bins
# Не удалять промежуточные .o, которые make строит по цепочке шаблонных правил.
.SECONDARY:

all: build

build: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(if $(V),,@echo "  компоновка  $@")
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@

# Одно правило компиляции и для src/, и для tests/.
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(if $(V),,@echo "  компиляция  $<")
	$(Q)$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_DIR)/%: $(OBJ_DIR)/tests/%.o $(LIB_OBJS)
	@mkdir -p $(@D)
	$(if $(V),,@echo "  компоновка  $@")
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@

run: build
	./$(TARGET) $(ARGS)

# Каждый тест — отдельный бинарник; код возврата 0 означает «ок».
# Вывод упавшего теста печатается целиком, у прошедшего — только при V=1.
test: $(TEST_BINS)
	@total=$(words $(TEST_BINS)); i=0; failed=0; \
	if [ $$total -eq 0 ]; then echo "Тестов нет."; exit 0; fi; \
	echo "Запуск тестов: $$total"; \
	for t in $(TEST_BINS); do \
	    i=$$((i + 1)); name=$$(basename $$t); name=$${name#test_}; \
	    printf '[%2d/%d] %-14s... ' $$i $$total "$$name"; \
	    start=$$(date +%s%N); \
	    if $(RUN) $$t > $$t.log 2>&1; then \
	        echo "ок ($$(( ($$(date +%s%N) - start) / 1000000 )) мс)"; \
	        if [ -n "$(V)" ]; then cat $$t.log; fi; \
	    else \
	        echo "ОШИБКА"; cat $$t.log; failed=$$((failed + 1)); \
	    fi; \
	done; \
	if [ $$failed -ne 0 ]; then echo "Провалено: $$failed из $$total"; exit 1; fi; \
	echo "Все тесты пройдены: $$total из $$total"

# Тот же make test, но в отдельном каталоге и с другими флагами: объектники
# обычной сборки и сборки с санитайзером не смешиваются.
tsan:
	@$(MAKE) --no-print-directory test BUILD_DIR=build/tsan CXXFLAGS="$(TSAN_CXXFLAGS)" \
	    RUN="setarch $$(uname -m) -R env TSAN_OPTIONS=halt_on_error=1 MAFIA_MASS=0.3"

# Только массовый тест (много партий на обоих движках) с подробным выводом.
# Объём: make mass MAFIA_MASS=3 — втрое больше партий.
mass: $(TEST_DIR)/test_mass
	./$(TEST_DIR)/test_mass

# Строгая сборка: всё (программа, тесты, поддельный сервер) с -Werror в
# отдельном каталоге — любое предупреждение остановит сборку.
strict:
	@$(MAKE) --no-print-directory all-bins BUILD_DIR=build/strict CXXFLAGS="$(CXXFLAGS) -Werror"
	@echo "Строгая сборка (-Werror): предупреждений нет"

# Всё для сдачи одной командой: строгая сборка, программа, тесты, тесты под TSan.
check: strict build fake-server
	@echo; echo "=== Тесты ==="
	@$(MAKE) --no-print-directory test
	@echo; echo "=== Тесты под ThreadSanitizer ==="
	@$(MAKE) --no-print-directory tsan
	@echo; echo "Проверка для сдачи пройдена."

# Поддельный сервис модели для ручной проверки ИИ-игроков без ключа (этап 16).
FAKE_SERVER := $(BUILD_DIR)/fake_llm_server

fake-server: $(FAKE_SERVER)

$(FAKE_SERVER): $(OBJ_DIR)/tools/fake_llm_server.o $(LIB_OBJS)
	@mkdir -p $(@D)
	$(if $(V),,@echo "  компоновка  $@")
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@

# Всё, что собирается: программа, поддельный сервер, тесты (для make strict).
all-bins: $(TARGET) $(FAKE_SERVER) $(TEST_BINS)

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(OBJ_DIR)/tools/fake_llm_server.d
