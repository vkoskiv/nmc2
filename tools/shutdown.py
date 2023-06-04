#!/usr/bin/python3
from websocket import create_connection
import json
import sys
from cred import *

if len(sys.argv) < 2:
	print('Usage: {} <url>'.format(sys.argv[0]))
	print('Example: {} wss://pixel.vkoskiv.com/ws'.format(sys.argv[0]))
	exit(0);

ws_url = sys.argv[1]

if not ws_url in uuids:
	print("UUID for {} not found in tools/cred.py".format(ws_url))
	exit(-1)

ws = create_connection(ws_url)

selected_uuid = uuids.get(ws_url)

payload = {'requestType': 'admin_cmd', 'userID': selected_uuid, 'cmd': {'action': 'shutdown'}}
ws.send(json.dumps(payload))
try:
	result = ws.recv()
	print("Received '%s'" % result)
except:
	print("Connection lost. I guess it worked, then?")
ws.close()
