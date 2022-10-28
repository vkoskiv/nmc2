#!/usr/bin/python3
import threading
import time
import json
import random
import struct
from websocket import create_connection

edge = 512

connections = []
users = 1
target_url = "ws://127.0.0.1:3001/ws"

for _ in range(users):
	try:
		ws = create_connection(target_url)
	except:
		print('Failed to create connection')
		continue
	ws.send(json.dumps({'requestType': 'initialAuth'}))
	while True:
		try:
			response = ws.recv()
		except:
			print("Didn't get reply. Perhaps kicked? Skipping.")
			break
		print("Got response: {}".format(response))
		resp = json.loads(response)
		if resp['rt'] == 'authSuccessful':
			print("Got user {}".format(resp['uuid']))
			connections.append((resp['uuid'], ws))
			break

# Each user runs this func on a bg thread
def random_pixels(uuid, socket):
	while True:
		time.sleep(random.uniform(0.2, 1.0))
		x = random.randint(0,edge)
		y = random.randint(100,edge)
		id = random.randint(0,16)
		socket.send_binary(struct.pack('B{}shhh'.format(len(uuid)), 4, uuid.encode(), x, y, id))

def getcanvas(uuid, socket):
	while True:
		time.sleep(random.uniform(10,60))
		socket.send(json.dumps({'requestType': 'getCanvas', 'userID': uuid}))

threads = []
for thing in connections:
	print("Booting pixel thread for {}".format(thing[0]))
	pixthread = threading.Thread(target=random_pixels, name=thing[0], args=thing)
	pixthread.start()
	threads.append(pixthread)

time.sleep(3000000)
