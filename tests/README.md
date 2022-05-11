~~~

Quick tests scripts I've been using to test effectiveness of rate limiting as well as the overall capabilites of this project.
These test scripts need a lot of work, here are some quick notes and a description:

* heavy_load.py:
- Tries to simulate lots and lots of hyperactive users drawing rapidly and constantly
refreshing the page. You can test the limits by disabling rate limiting (uncomment DISABLE_RATE_LIMITING in main.c and recompile)
- Requires you increase the max_users_per_ip limit in params.json because all the test users are created from 127.1

* spam_tiles.py:
- Tries to spam tiles with just a loop starting from top left.
- Good test of rate limiting - Bump the user tile count in the db, and experiment with/without rate limiting enabled.

* spam_tiles_random.py:
- Same as above, but randomly generates X,Y,colorID params for each tile place.
- Without rate limiting, this fills the canvas up real fast! 
- Useful for filling a canvas with random noise to create a worst-case scenario for the zlib canvas encoder.

Caveats:

- These scripts are hastily put together. The spam_* scripts require you place a valid uuid in them before running them.
- Test on your own local instance, please. You are free to write bots to draw cool things on pixel.vkoskiv.com, but refrain from spamming just to mess things up. Thanks!

~~~
