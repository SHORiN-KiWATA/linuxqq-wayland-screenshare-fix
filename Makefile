# linuxqq-wayland-fix
#
#   make
#   make install DESTDIR=... PREFIX=/usr
#
# LIBEXECDIR 下放两个注入库；启动脚本在安装时写入它的绝对路径。

NAME       := linuxqq-wayland-fix
VERSION    ?= $(or $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//'),0.0.0)

PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/$(NAME)
DATADIR    ?= $(PREFIX)/share
DOCDIR     ?= $(DATADIR)/doc/$(NAME)

CC         ?= cc
CFLAGS     ?= -O2 -g
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

# 屏幕共享修复：只用 libpulse / libpipewire 的头文件，运行时不依赖它们
SS_CFLAGS  := $(shell $(PKG_CONFIG) --cflags gio-unix-2.0 libpulse libpipewire-0.3)
SS_LIBS    := $(shell $(PKG_CONFIG) --libs gio-unix-2.0)
# 剪贴板修复
CB_CFLAGS  := $(shell $(PKG_CONFIG) --cflags x11 wayland-client)
CB_LIBS    := $(shell $(PKG_CONFIG) --libs x11 wayland-client)

SS_LIB     := libqq-wl-portal.so
CB_LIB     := libqq-clipbridge.so
SH_LIB     := libqq-screenshot.so
BF_LIB     := libqq-borderfix.so
# H.264 编码器替换（NVENC）：默认不生效，QQ_NVENC=1 挂钩、QQ_NVENC_ACTIVE=1 才真正用 NVENC。
# 需要 ffnvcodec 的 nvEncodeAPI.h（Debian/Ubuntu: libffmpeg-nvenc-dev；Arch: nv-codec-headers）；
# 头文件不在就不编这个库，其余目标不受影响。
NVENC_INC  ?= /usr/include
NVENC_HDR  := $(NVENC_INC)/ffnvcodec/nvEncodeAPI.h
NV_LIB     := libqq-nvenc.so
ifeq ($(wildcard $(NVENC_HDR)),)
NVENC_BUILD :=
$(info 提示：找不到 $(NVENC_HDR)，all/install 会跳过 $(NV_LIB)。)
$(info       要启用：Debian/Ubuntu 装 libffmpeg-nvenc-dev，Arch 装 nv-codec-headers，)
$(info       或者用 make NVENC_INC=<含 ffnvcodec/ 的 include 目录> 指定自己的头文件位置。)
else
NVENC_BUILD := 1
endif
SHOT_HELPER := qq-screenshot-helper
SHOT_HELPER_DESKTOP := $(SHOT_HELPER).desktop
CMD        := $(NAME)
CB_PROTOCOLS := ext-data-control-v1 wlr-data-control-unstable-v1
CB_GEN_H   := $(CB_PROTOCOLS:%=build/%-client-protocol.h)
CB_GEN_C   := $(CB_PROTOCOLS:%=build/%-protocol.c)

ALL_LIBS := $(SS_LIB) $(CB_LIB) $(SH_LIB) $(BF_LIB) $(if $(NVENC_BUILD),$(NV_LIB))

all: $(ALL_LIBS) $(SHOT_HELPER) $(SHOT_HELPER_DESKTOP) $(CMD)

build/qq-wl-portal.o: src/qq-wl-portal.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra -Wno-nonnull-compare \
	    -DQQWL_VERSION='"$(VERSION)"' $(SS_CFLAGS) -c -o $@ $<

build/dlsym_trampoline.o: src/dlsym_trampoline.S
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(ASFLAGS) -fPIC -c -o $@ $<

$(SS_LIB): build/qq-wl-portal.o build/dlsym_trampoline.o
	$(CC) $(LDFLAGS) -shared -Wl,-z,defs -o $@ $^ $(SS_LIBS) -ldl

build/%-client-protocol.h: protocol/%.xml
	@mkdir -p build
	$(WAYLAND_SCANNER) client-header $< $@

build/%-protocol.c: protocol/%.xml
	@mkdir -p build
	$(WAYLAND_SCANNER) private-code $< $@

$(CB_LIB): src/qq-clipbridge.c $(CB_GEN_H) $(CB_GEN_C)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Ibuild $(CB_CFLAGS) \
	    $(LDFLAGS) -shared -Wl,-z,defs -o $@ src/qq-clipbridge.c $(CB_GEN_C) $(CB_LIBS) -lpthread -ldl

# 截图修复：同样用 X11 与 wayland-client（wlr-screencopy）；KDE 下 fork/exec helper 调 KWin ScreenShot2
$(SH_LIB): src/qq-screenshot.c build/wlr-screencopy-unstable-v1-client-protocol.h build/wlr-screencopy-unstable-v1-protocol.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra -Ibuild $(CB_CFLAGS) \
	    -DQQ_SCREENSHOT_HELPER='"$(LIBEXECDIR)/$(SHOT_HELPER)"' \
	    $(LDFLAGS) -shared -Wl,-z,defs -o $@ src/qq-screenshot.c build/wlr-screencopy-unstable-v1-protocol.c $(CB_LIBS) -ldl

# KDE 截图 helper：替 QQ 调 KWin ScreenShot2（kde 模式）或 portal 截图（portal 模式）；
# 配套 .desktop 声明受限接口（KWin 按 exe 路径鉴权）
$(SHOT_HELPER): src/qq-screenshot-helper.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra $(SS_CFLAGS) -o $@ $< $(SS_LIBS) -lz

$(SHOT_HELPER_DESKTOP): $(SHOT_HELPER_DESKTOP).in
	sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' $< > $@

# 共享边框隐藏：socket 层拦 Wayland 出站流量，把「屏幕共享」全屏边框的 buffer 摘掉
$(BF_LIB): src/qq-borderfix.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra \
	    $(LDFLAGS) -shared -Wl,-z,defs -o $@ src/qq-borderfix.c -lpthread -ldl

# H.264 编码器替换（NVENC）：默认不生效，QQ_NVENC=1 时在 ppapi 里挂钩 AVSDK 的编码器工厂
$(NV_LIB): src/qq-nvenc.c
	@if [ ! -f "$(NVENC_HDR)" ]; then \
	    echo "make: 编 $(NV_LIB) 需要 $(NVENC_HDR)"; \
	    echo "      请装 libffmpeg-nvenc-dev（Debian/Ubuntu）或 nv-codec-headers（Arch），"; \
	    echo "      或用 make NVENC_INC=<含 ffnvcodec/ 的 include 目录> 指定位置。"; \
	    exit 1; fi
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(NVENC_INC) -fPIC -Wall -Wextra \
	    $(LDFLAGS) -shared -Wl,-z,defs -o $@ src/qq-nvenc.c -lpthread -ldl

$(CMD): $(CMD).in
	sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@
	chmod +x $@

install: all
	install -Dm755 $(SS_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(SS_LIB)
	install -Dm755 $(CB_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(CB_LIB)
	install -Dm755 $(SH_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(SH_LIB)
	install -Dm755 $(BF_LIB)         $(DESTDIR)$(LIBEXECDIR)/$(BF_LIB)
	if [ -n "$(NVENC_BUILD)" ]; then install -Dm755 $(NV_LIB) $(DESTDIR)$(LIBEXECDIR)/$(NV_LIB); fi
	install -Dm755 $(SHOT_HELPER)     $(DESTDIR)$(LIBEXECDIR)/$(SHOT_HELPER)
	install -Dm644 $(SHOT_HELPER_DESKTOP) $(DESTDIR)$(DATADIR)/applications/$(SHOT_HELPER_DESKTOP)
	install -Dm755 $(CMD)            $(DESTDIR)$(BINDIR)/$(CMD)
	install -Dm644 $(CMD).desktop    $(DESTDIR)$(DATADIR)/applications/$(CMD).desktop
	install -Dm644 README.md         $(DESTDIR)$(DOCDIR)/README.md
	for f in docs/*.md; do install -Dm644 "$$f" "$(DESTDIR)$(DOCDIR)/$$f"; done
	install -Dm644 LICENSE           $(DESTDIR)$(DATADIR)/licenses/$(NAME)/LICENSE

clean:
	rm -rf build src/*.o $(SS_LIB) $(CB_LIB) $(SH_LIB) $(BF_LIB) $(NV_LIB) $(SHOT_HELPER) $(SHOT_HELPER_DESKTOP) $(CMD)

print-version:
	@echo $(VERSION)

.PHONY: all install clean print-version
