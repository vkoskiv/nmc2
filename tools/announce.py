#!/usr/bin/python3
from websocket import create_connection
import json
import sys
from cred import *

if 'Place' in admin_uuid:
	print("Substitute a valid uuid in tools/cred.py before running these scripts")
	exit(0)

if len(sys.argv) < 2:
	print('Usage: {} <url> <message>'.format(sys.argv[0]))
	print('Example: {} ws://localhost:3001/ws \'Hello, world!\''.format(sys.argv[0]))
	exit(0)

ws = create_connection(sys.argv[1])
payload = {'requestType': 'admin_cmd', 'userID': admin_uuid, 'cmd': {'action': 'message', 'message': sys.argv[2]}}
ws.send(json.dumps(payload))
result = ws.recv()
print("Received '%s'" % result)
ws.close()
