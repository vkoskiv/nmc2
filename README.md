~~~

nmc2 - new backend websocket server for No Man's Canvas

Dependencies: sqlite3, uuid, zlib, libbsd, gcc with nested function support
You can try this, but it's linux so you'll probably have to google it anyway:

Ubuntu: sudo apt install uuid-dev libsqlite3-dev zlib1g-dev libbsd-dev
Arch: sudo pacman -S sqlite zlib

To build: make -j4
To run: bin/nmc2

macOS Caveat:
Change cc -> gcc-11 in Makefile before compiling

Web client can be found here: https://github.com/EliasHaaralahti/No-Mans-Canvas-Client

The original Swift 3.1 + Vapor 2 implementation is here:
https://github.com/vkoskiv/NoMansCanvas

You can find some runtime config options in params.json:

* new_db_canvas_size - Edge length of square canvas when generating a new one.
* getcanvas_max_rate - Max rate of getCanvas requests
* getcanvas_per_seconds - Per this many seconds^
* setpixel_max_rate - Max rate of postTile request
* setpixel_per_seconds - Per this many seconds^
* max_users_per_ip - Try to limit the amount of users per host to this amount
* canvas_save_interval_sec - Save the canvas to db once every this many seconds
* websocket_ping_interval_sec - Ping active websockets every this many seconds
* admin_uuid - Doesn't have to be an uuid. Just the password to invoke admin commands at runtime (see tools directory)
* listen_url - Address, port for listening
* dbase_file - Name of the database file to use
* colors     - Array of colors of format [R, G, B, id]. id has to be unique. Order in array determines which order they show up in the client.

You can reload the config file at runtime by sending SIGUSR1 to the running process:
`kill -SIGUSR1 $(pidof nmc2)`

You can tell the running server to save a backup of the database under backups/ by sending SIGUSR2.
The script backup_db.sh shows you a nice way to run backups with cron

~~~
