# Star PI Dashboard Backend

Flask server to host the frontend and provide telemetry APIs.

## Setup

1. Create a virtual environment:
```bash
python3 -m venv venv
source venv/bin/activate  # On Linux/Mac
```

2. Install dependencies:
```bash
pip install -r requirements.txt
```

3. Build the frontend first:
```bash
cd ../Frontend
npm run build
cd ../Backend
```

4. Upload the Frontend to the Hoster

5. Run the backend server:
```bash
python server.py
```

The server will run on `http://localhost:5000`

## API Endpoints

- `GET /api/flights` - List all flights
- `GET /api/telemetry/<flight_id>` - Get telemetry data for a specific flight
