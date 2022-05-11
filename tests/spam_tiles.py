#!/usr/bin/python3
import time
import json
import random
from websocket import create_connection

uuid = '<generate a uuid and put here>'

ws = create_connection("ws://localhost/ws")
ws.send(json.dumps({'requestType': 'auth', 'userID': uuid}))
result = ws.recv()
i = 0

pixels = 512*512

while True:
	for y in range(512):
		for x in range(512):
			time.sleep(0.001)
			i += 1
			id = i % 15
			ws.send(json.dumps({'requestType': 'postTile', 'userID': uuid, 'X': x, 'Y': y, 'colorID': str(id)}))
			result = ws.recv()
			if "error" in result:
				continue
			print("Set pixel")


print("Done")
time.sleep(5)

ws.close()
