#!/vsta/bin/sh
set -e
set -x
if test \( "$1" = "all" \) -o \( "$1" = "install" \) -o \( "$1" = "" \)
then
	cd lib
	make libusr.a
	cd ..
fi
cd include
make $*
cd ..
cd os/make
make $*
cd ../..
cd lib
make $*
cd ..
cd srv
cd bfs
make $*
cd cmd
make $*
cd ..
cd ..
cd namer
make $*
cd ..
cd swap
make $*
cd ..
cd env
make $*
cd ..
cd dos
make $*
cd ..
cd tmpfs
make $*
cd ..
cd pipe
make $*
cd ..
cd vstafs
make $*
cd cmd
make $*
cd ..
cd ..
cd devnull
make $*
cd ..
cd cdfs
make $*
cd ..
cd proc
make $*
cd cmd
make $*
cd ..
cd ..
cd sema
make $*
cd ..
cd mach
cd cons2
make $*
cd ..
cd fd
make $*
cd ..
cd wd
make $*
cd ..
cd rs232
make $*
cd ..
cd scsi
make $*
cd ..
cd mouse
make $*
cd ..
cd par
make $*
cd ..
cd joystick
make $*
cd ..
cd nvram
make $*
cd ..
cd ne
make $*
cd ..
cd ..
cd ..
cd bin/cmds
make $*
cd ../login
make $*
cd ../init
make $*
cd ../testsh
make $*
cd ../time
make $*
cd ../roff
make $*
cd ../adb
make $*
cd ../ash
make $*
cd ../..
exit 0
