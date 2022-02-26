from websocket import create_connection

ws = create_connection("ws://localhost:3001/ws")

ws.send('{"requestType": "admin_cmd", "userID": "94E9AD2E-2D24-46F9-9612-A258BACFC5A2", "cmd": {"action": "shutdown"}}')
result = ws.recv()
print("Received '%s'" % result)

ws.close()
