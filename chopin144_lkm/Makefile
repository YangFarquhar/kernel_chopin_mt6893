obj-m += chopin144_experimental.o

ccflags-y += -I$(src)/../drivers/gpu/drm/mediatek
ccflags-y += -I$(src)/../drivers/gpu/drm/mediatek/mi_disp
ccflags-y += -I$(src)/../include

ccflags-y += -I$(src)/../drivers/misc/mediatek/mmdvfs
ccflags-y += -I$(src)/../drivers/misc/mediatek/mmp

# Linux 4.14 arm64 module loader does not support GOT relocations.
# Disable compiler features that can introduce GOT references.
CFLAGS_chopin144_experimental.o += -fno-pic -fno-pie -fno-stack-protector
