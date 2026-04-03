import asyncio
import json
from fastapi import FastAPI, WebSocket
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse
import uvicorn
import frameparser

app = FastAPI()

async def get_parsed_data(dev):
    """
    This replaces the blocking 'while True' loop.
    You read your serial port, pass it to your parser, and yield the result.
    """
    while True:
        # NOTE: Replace this mock data with your actual parser logic
        # data = your_parser_module.read_and_parse()
        import random
        mock_value = random.randint(0, 100) 
        
        # Yield the data as a dictionary. 
        # The asyncio.sleep mimics a ~60Hz hardware update rate.
        await asyncio.sleep(0.016) 
        yield {"sensor_value": mock_value}

PORT_MAP = {
    "blt":"/dev/rfcomm0",
    "serial":"/dev/ttyUSB0",
    "debug":"/dev/null",
}


def health_monitoring_exec(ws, dev_key):
    port_path = PORT_MAP.get(dev_key)
    if not port_path:
        ws.send_text("Error: Invalid device.")
        ws.close(code=1000)
        return

    # open serial
    try:
        ser = serial.Serial(port_path, 115200, timeout=1100)
        ser.reset_input_buffer()
    except Exception as e:
        print(f"Connection error on {port_path}: {e}")
        await ws.close(code=1000)
        if 'ser' in locals() and ser.is_open:
            await ser.close()
        return

    if 'ser' in locals() and ser.is_open:
        await ser.close()


@app.websocket("/raw_ws")
async def websocket_endpoint(ws:WebSocket, dev:str="debug"):
    port_path = PORT_MAP.get(dev)
    if not port_path:
        await ws.close(code=1000)
        return

    await ws.accept()

    print(f"Dashboard Connected. Streaming Data from {dev}...")

    try:
        while True:
            await ws.send_text(health_monitoring_exec(port_path))
            await asyncio.sleep(0.016)
    except Exception as e:
        print(f"Connection closed: {e}.")

@app.get("/")
async def serve_dashboard():
    with open("index.html", "r") as f:
        return HTMLResponse(f.read())

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=5000, log_level="warning")
