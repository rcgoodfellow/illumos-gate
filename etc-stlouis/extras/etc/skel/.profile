export PATH=/usr/bin:/usr/sbin:/sbin:/tmp/bin:/tmp/usr/bin:/tmp/usr/sbin:/tmp/sbin:/tmp/usr/xpg6/bin:/opt/local/bin
export LD_LIBRARY_PATH_32=/tmp/lib:/tmp/usr/lib
export LD_LIBRARY_PATH_64=/tmp/lib/64:/tmp/usr/lib/64

# Work around missing terminfo
if [ "$TERM" = "alacritty" ]; then
	TERM=xterm-256color
fi
