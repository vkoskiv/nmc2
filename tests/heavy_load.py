#!/usr/bin/python3
import threading
import time
import json
import random
from websocket import create_connection

# Ideally fetch the canvas once first and get these
edge = 512
pixels = edge * edge

connections = []
users = 50
target_url = "ws://localhost:3001/ws"

for _ in range(users):
	try:
		ws = create_connection(target_url)
	except:
		print('Failed to create connection')
		continue
	ws.send(json.dumps({'requestType': 'initialAuth'}))
	while True:
		response = ws.recv()
		print("Got response: {}".format(response))
		resp = json.loads(response)
		if resp['rt'] == 'authSuccessful':
			print("Got user {}".format(resp['uuid']))
			connections.append((resp['uuid'], ws))
			break

# Each user runs this func on a bg thread
def random_pixels(uuid, socket):
	while True:
		time.sleep(random.uniform(0.2, 30.0))
		x = random.randint(0,edge)
		y = random.randint(0,edge)
		id = random.randint(0,16)
		socket.send(json.dumps({'requestType': 'postTile', 'userID': uuid, 'X': x, 'Y': y, 'colorID': str(id)}))

def getcanvas(uuid, socket):
	while True:
		time.sleep(random.uniform(10,120))
		socket.send(json.dumps({'requestType': 'getCanvas', 'userID': uuid}))

threads = []
for thing in connections:
	print("Booting getcanvas thread for {}".format(thing[0]))
	thread = threading.Thread(target=getcanvas, name=thing[0], args=thing)
	thread.start()
	threads.append(thread)
	print("Booting pixel thread for {}".format(thing[0]))
	pixthread = threading.Thread(target=random_pixels, name=thing[0], args=thing)
	pixthread.start()
	threads.append(pixthread)

time.sleep(20)
