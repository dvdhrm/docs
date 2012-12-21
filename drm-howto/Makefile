all:
	gcc -o modeset modeset.c `pkg-config --cflags --libs libdrm` -Wall -O0 -g
	gcc -o modeset-double-buffered modeset-double-buffered.c `pkg-config --cflags --libs libdrm` -Wall -O0 -g
	gcc -o modeset-vsync modeset-vsync.c `pkg-config --cflags --libs libdrm` -Wall -O0 -g
