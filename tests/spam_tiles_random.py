#!/usr/bin/python3
import time
import json
import random
from websocket import create_connection

uuid = '<generate a uuid and put here>'

ws = create_connection("ws://localhost:3001/ws")
ws.send(json.dumps({'requestType': 'auth', 'userID': uuid}))
result = ws.recv()
i = 0

edge = 512
pixels = edge*edge

while True:
	x = random.randint(0,edge)
	y = random.randint(0,edge)
	id = random.randint(0,16)
	ws.send(json.dumps({'requestType': 'postTile', 'userID': uuid, 'X': x, 'Y': y, 'colorID': str(id)}))
	result = ws.recv()
	if "error" in result:
		continue
	print("Set pixel");

print("Done")
time.sleep(5)

ws.close()
