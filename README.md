~~~

nmc2 - new backend websocket server for No Man's Canvas

You can try this, but it's linux so you'll probably have to google it anyway.
Ubuntu: sudo apt install uuid-dev libsqlite3-dev
Arch: sudo pacman -S sqlite

Dependencies: sqlite3, uuid, gcc with nested function support
To build: make -j4
To run: bin/nmc2

macOS Caveat:
Change cc -> gcc-11 in Makefile before compiling

Web client: https://github.com/EliasHaaralahti/No-Mans-Canvas-Client

The original Swift 3.1 + Vapor 2 implementation is here:
https://github.com/vkoskiv/NoMansCanvas

~~~
