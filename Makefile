# pw-mpris-visualcard / native —— 单进程 C++ 渲染器
# 依赖全是发行版系统库，没有第三方包管理器依赖。

CXX      ?= g++
PKGS     := cairo pangocairo libpipewire-0.3 sdbus-c++ libcurl gdk-pixbuf-2.0 glib-2.0
# -O3 -march=native 对旋转热循环收益明显（实测整帧 -22%）。
# 代价是二进制绑死本机指令集；换机器跑或要分发就用 make PORTABLE=1。
ifeq ($(PORTABLE),1)
  ARCHFLAGS :=
else
  ARCHFLAGS := -march=native
endif

CXXFLAGS ?= -O3 -g -funroll-loops $(ARCHFLAGS)
CXXFLAGS += -std=c++20 -Wall -Wextra $(EXTRA_CXXFLAGS) $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

TARGET   := pw-mpris-visualcard-native
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:.cpp=.o)
DEP      := $(OBJ:.o=.d)

# systemd 用户服务：unit 是模板，@REPO@ / @ARGS@ 在安装时替换成实际值
UNIT         := pw-mpris-visualcard.service
UNIT_DIR     ?= $(HOME)/.config/systemd/user
SERVICE_ARGS ?= --node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3

.PHONY: all clean dump run install-service uninstall-service

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 不出画面，直接渲染一张 PNG，用来调版式
dump: $(TARGET)
	./$(TARGET) --demo --lyrics 4 --time 1 --album 1 --dump /tmp/card.png

run: $(TARGET)
	./$(TARGET)

# 渲染 unit 并安装到用户 systemd 目录；不自动 enable/启动，避免改动机器既有状态
install-service: $(TARGET)
	@mkdir -p $(UNIT_DIR)
	sed -e 's|@REPO@|$(CURDIR)|g' -e 's|@ARGS@|$(SERVICE_ARGS)|g' \
	    $(UNIT) > $(UNIT_DIR)/$(UNIT)
	systemctl --user daemon-reload
	@echo "已安装 $(UNIT_DIR)/$(UNIT)"
	@echo "启用并启动： systemctl --user enable --now pw-mpris-visualcard"
	@echo "改过参数后： systemctl --user restart pw-mpris-visualcard"

uninstall-service:
	-systemctl --user disable --now pw-mpris-visualcard
	rm -f $(UNIT_DIR)/$(UNIT)
	systemctl --user daemon-reload
	@echo "已卸载 $(UNIT_DIR)/$(UNIT)"

clean:
	rm -f $(OBJ) $(DEP) $(TARGET)

-include $(DEP)
