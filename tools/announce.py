#!/usr/bin/python3
from websocket import create_connection
import json
import sys
from cred import *

if len(sys.argv) < 3:
	print('Usage: {} <url> <message>'.format(sys.argv[0]))
	print('Example: {} ws://localhost:3001/ws \'Hello, world!\''.format(sys.argv[0]))
	exit(0)

ws_url = sys.argv[1]

if not ws_url in uuids:
	print("UUID for {} not found in tools/cred.py".format(ws_url))
	exit(-1)

selected_uuid = uuids.get(ws_url)
ws = create_connection(ws_url)
payload = {'requestType': 'admin_cmd', 'userID': selected_uuid, 'cmd': {'action': 'message', 'message': sys.argv[2]}}
ws.send(json.dumps(payload))
result = ws.recv()
print("Received '%s'" % result)
ws.close()
