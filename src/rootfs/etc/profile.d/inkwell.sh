# Shell setup for Terminal and SSH sessions. An e-ink panel redraws slowly and
# keeps ghosts, so programs are asked for plain text: no colours, no progress
# bars, nothing that redraws on a timer.
export NO_COLOR=1
export EDITOR=nano PAGER=less MANPAGER=less LESS=-iR

# The terminal programs keep their settings and data on the Kindle drive, so
# they survive an update, which replaces alpine.img and with it /root.
export XDG_CONFIG_HOME=/mnt/us/linux/config XDG_DATA_HOME=/mnt/us/linux/data
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" 2>/dev/null
# StarDict dictionaries for sdcv (.ifo, .idx and .dict.dz files) go in the
# drive's stardict folder.
export STARDICT_DATA_DIR=/mnt/us/stardict

# Our defaults for programs that have no settings yet. Once copied they are
# the user's, and an update leaves them alone.
D=/usr/local/share/inkwell/config
for f in $(cd "$D" && find . -type f); do
    [ -e "$XDG_CONFIG_HOME/$f" ] || { mkdir -p "$(dirname "$XDG_CONFIG_HOME/$f")" && cp "$D/$f" "$XDG_CONFIG_HOME/$f"; }
done 2>/dev/null
unset D f

alias htop='htop -d 100' # redraw every 10 seconds instead of every second
alias apk='apk --no-progress'
# Weather as plain narrow text: "weather" for where the IP is, "weather Lviv".
weather() { curl -s "wttr.in/$1?T0Qn"; }
