# Makefile for the drivers module

# Set the module name
MODULE := fs

FILES := vfs.c tfs.c filesystems.c flatfs.c newfs.c

SRC += $(patsubst %, $(MODULE)/%, $(FILES))
