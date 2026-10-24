# SketchOS 用户态程序构建
#
# 通常由 scripts/userspace.sh 调用：
#   make -C userspace CC=$SKCC BUILD=$WORK/build/userspace
# 产物在 $(BUILD)/bin 下（全部是静态链接的 i686 程序）。
#
# 目录约定（新增 .c 文件会被自动编译，不需要改这个 Makefile）：
#   common/*.c          公共工具函数（字符串、/proc 解析……），链接进所有程序
#   init/*.c            → sinit      1 号进程
#   shell/*.c           → sksh       命令行 shell
#   skbox/*.c           → skbox      多合一工具箱（ls、cat、ps……），每个 applet 一个文件
#   gui/libsketch/*.c   → libsketch.a  图形库（绘图、手绘图元、字体、控件、客户端）
#   gui/sketchd/*.c     → sketchd    显示服务器 + 窗口管理器 + 桌面
#   gui/apps/<名字>/*.c → <名字>     每个子目录是一个图形应用
#   gui/tests/*.c       → 宿主机离线渲染测试（make host-tests）

CC      ?= sk-cc
HOSTCC  ?= gcc
BUILD   ?= ../build/userspace
VERSION ?= 1.1

WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 $(WARN) -ffunction-sections -fdata-sections \
           -Icommon -Igui/include -I$(BUILD)/gen -DSKETCHOS_VERSION=\"$(VERSION)\"
LDFLAGS ?=
LDFLAGS += -Wl,--gc-sections -s
LDLIBS  := -lm

BIN := $(BUILD)/bin
OBJ := $(BUILD)/obj

common_src    := $(wildcard common/*.c)
sinit_src     := $(wildcard init/*.c)
sksh_src      := $(wildcard shell/*.c)
skbox_src     := $(wildcard skbox/*.c)
libsketch_src := $(wildcard gui/libsketch/*.c)
sketchd_src   := $(wildcard gui/sketchd/*.c)
app_dirs      := $(patsubst gui/apps/%/,%,$(sort $(dir $(wildcard gui/apps/*/*.c))))
headers       := $(wildcard common/*.h gui/include/sketch/*.h gui/libsketch/*.h gui/sketchd/*.h shell/*.h skbox/*.h init/*.h gui/apps/*/*.h)

obj = $(patsubst %.c,$(OBJ)/%.o,$(1))

PROGRAMS := $(if $(sinit_src),$(BIN)/sinit) \
            $(if $(sksh_src),$(BIN)/sksh) \
            $(if $(skbox_src),$(BIN)/skbox) \
            $(if $(sketchd_src),$(BIN)/sketchd) \
            $(foreach a,$(app_dirs),$(BIN)/$(a))

.PHONY: all clean host-tests list-apps sinit sksh skbox sketchd libsketch $(app_dirs)
all: $(PROGRAMS) $(if $(skbox_src),$(BUILD)/skbox.applets)

# 快捷目标：make sksh / make skbox / make skterm ……只编译一个程序
sinit:     $(BIN)/sinit
sksh:      $(BIN)/sksh
skbox:     $(BIN)/skbox $(BUILD)/skbox.applets
sketchd:   $(BIN)/sketchd
libsketch: $(BUILD)/libsketch.a
$(foreach a,$(app_dirs),$(eval $(a): $(BIN)/$(a)))

list-apps:
	@echo $(app_dirs)

# 头文件一改就全部重编（项目很小，简单可靠）
$(OBJ)/%.o: %.c $(headers)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# skbox 的对象文件额外依赖自动生成的 applet 表
$(OBJ)/skbox/%.o: skbox/%.c $(headers) $(BUILD)/gen/skbox_applets.inc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- skbox applet 表：从源码里的 SKBOX_APPLET(名字, "说明") 标记生成 ----
$(BUILD)/gen/skbox_applets.inc: $(skbox_src)
	@mkdir -p $(dir $@)
	@grep -h -E '^SKBOX_APPLET\(' $(skbox_src) /dev/null | sort -u | \
	   sed -E 's/^SKBOX_APPLET\(/SKBOX_ENTRY(/; s/\)[[:space:]]*;?[[:space:]]*$$/)/' > $@.tmp
	@mv $@.tmp $@

$(BUILD)/skbox.applets: $(BUILD)/gen/skbox_applets.inc
	@sed -E 's/^SKBOX_ENTRY\(([A-Za-z0-9_]+),.*/\1/' $< > $@

# ---- 库 ----
$(BUILD)/libcommon.a: $(call obj,$(common_src))
	@mkdir -p $(dir $@)
	rm -f $@; ar rcs $@ $^

$(BUILD)/libsketch.a: $(call obj,$(libsketch_src))
	@mkdir -p $(dir $@)
	rm -f $@; ar rcs $@ $^

# ---- 程序 ----
$(BIN)/sinit: $(call obj,$(sinit_src)) $(BUILD)/libcommon.a
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $(call obj,$(sinit_src)) $(BUILD)/libcommon.a $(LDLIBS)

$(BIN)/sksh: $(call obj,$(sksh_src)) $(BUILD)/libcommon.a
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $(call obj,$(sksh_src)) $(BUILD)/libcommon.a $(LDLIBS)

$(BIN)/skbox: $(call obj,$(skbox_src)) $(BUILD)/libcommon.a
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $(call obj,$(skbox_src)) $(BUILD)/libcommon.a $(LDLIBS)

$(BIN)/sketchd: $(call obj,$(sketchd_src)) $(BUILD)/libsketch.a $(BUILD)/libcommon.a
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $(call obj,$(sketchd_src)) $(BUILD)/libsketch.a $(BUILD)/libcommon.a $(LDLIBS)

define APP_RULE
$(BIN)/$(1): $(call obj,$(wildcard gui/apps/$(1)/*.c)) $(BUILD)/libsketch.a $(BUILD)/libcommon.a
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $$@ $(call obj,$(wildcard gui/apps/$(1)/*.c)) $(BUILD)/libsketch.a $(BUILD)/libcommon.a $(LDLIBS)
endef
$(foreach a,$(app_dirs),$(eval $(call APP_RULE,$(a))))

# ---- 宿主机离线渲染测试：用本机 gcc 编译图形库，把画面输出成 PPM 图片 ----
HOST     := $(BUILD)/host
host_src := $(wildcard gui/tests/*.c)
HOST_CFLAGS := -O2 -g -std=gnu11 $(WARN) -Icommon -Igui/include -I$(BUILD)/gen -DSKETCH_HOST_TEST
host-tests: $(patsubst gui/tests/%.c,$(HOST)/%,$(host_src))

$(HOST)/obj/%.o: %.c $(headers)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -c -o $@ $<

$(HOST)/libsketch.a: $(patsubst %.c,$(HOST)/obj/%.o,$(libsketch_src) $(common_src))
	rm -f $@; ar rcs $@ $^

$(HOST)/%: gui/tests/%.c $(HOST)/libsketch.a $(headers)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $< $(HOST)/libsketch.a -lm

clean:
	rm -rf $(BUILD)
