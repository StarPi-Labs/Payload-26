from flask import Flask, send_from_directory, jsonify, request, abort
from flask_cors import CORS
from werkzeug.utils import secure_filename
from datetime import datetime
import os
import json
import re

# Configuration
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(BASE_DIR, 'data')
ALLOWED_VIDEO_EXTENSIONS = {'mp4', 'avi', 'mov', 'mkv'}
ALLOWED_DATA_EXTENSIONS = {'txt', 'csv', 'log'}

app = Flask(__name__, static_folder='../Frontend/dist')
CORS(app)

# Ensure data directory exists
os.makedirs(DATA_DIR, exist_ok=True)


# ============================================================================
# UTILITY FUNCTIONS
# ============================================================================

def allowed_file(filename, allowed_extensions):
    """Check if file has an allowed extension"""
    return '.' in filename and filename.rsplit('.', 1)[1].lower() in allowed_extensions


def parse_sensor_data(file_path):
    """
    Parse sensor data from a TXT file.
    Expected format (one reading per line, comma or space separated):
    timestamp,altitude,velocity,acceleration,temperature,pressure,humidity,gps_lat,gps_lon,pitch,roll,yaw
    
    Or with headers in first line.
    """
    telemetry = []
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    if not lines:
        return telemetry
    
    # Check if first line is a header
    first_line = lines[0].strip()
    start_idx = 0
    
    # Detect delimiter (comma, semicolon, tab, or space)
    if ',' in first_line:
        delimiter = ','
    elif ';' in first_line:
        delimiter = ';'
    elif '\t' in first_line:
        delimiter = '\t'
    else:
        delimiter = None  # Will split on whitespace
    
    # Check if first line contains non-numeric values (header)
    parts = first_line.split(delimiter) if delimiter else first_line.split()
    try:
        float(parts[0])
    except ValueError:
        start_idx = 1  # Skip header
    
    for i, line in enumerate(lines[start_idx:]):
        line = line.strip()
        if not line:
            continue
        
        parts = line.split(delimiter) if delimiter else line.split()
        
        try:
            # Map data to telemetry structure
            # Adapt this based on your actual sensor data format
            data_point = {
                'time': float(parts[0]) if len(parts) > 0 else i * 0.1,
                'altitude': float(parts[1]) if len(parts) > 1 else 0,
                'velocity': float(parts[2]) if len(parts) > 2 else 0,
                'horizontalVelocity': float(parts[3]) if len(parts) > 3 else 0,
                'acceleration': float(parts[4]) if len(parts) > 4 else 0,
                'accelerationX': float(parts[5]) if len(parts) > 5 else 0,
                'accelerationY': float(parts[6]) if len(parts) > 6 else 0,
                'accelerationZ': float(parts[7]) if len(parts) > 7 else 0,
                'temperature': float(parts[8]) if len(parts) > 8 else 20,
                'pressure': float(parts[9]) if len(parts) > 9 else 101.3,
                'humidity': float(parts[10]) if len(parts) > 10 else 50,
                'gpsLat': float(parts[11]) if len(parts) > 11 else 0,
                'gpsLon': float(parts[12]) if len(parts) > 12 else 0,
                'pitch': float(parts[13]) if len(parts) > 13 else 0,
                'roll': float(parts[14]) if len(parts) > 14 else 0,
                'yaw': float(parts[15]) if len(parts) > 15 else 0,
            }
            telemetry.append(data_point)
        except (ValueError, IndexError) as e:
            print(f"Warning: Could not parse line {i + start_idx + 1}: {line}")
            continue
    
    return telemetry


def get_flight_info(flight_folder):
    """Get information about a flight from its folder"""
    folder_name = os.path.basename(flight_folder)
    
    # Count videos
    videos_dir = os.path.join(flight_folder, 'videos')
    videos = []
    if os.path.exists(videos_dir):
        videos = [f for f in os.listdir(videos_dir) 
                  if allowed_file(f, ALLOWED_VIDEO_EXTENSIONS)]
    
    # Check for telemetry data
    telemetry_dir = os.path.join(flight_folder, 'telemetry')
    has_telemetry = False
    telemetry_files = []
    if os.path.exists(telemetry_dir):
        telemetry_files = [f for f in os.listdir(telemetry_dir) 
                          if allowed_file(f, ALLOWED_DATA_EXTENSIONS)]
        has_telemetry = len(telemetry_files) > 0
    
    # Calculate duration from telemetry if available
    duration = 0
    if has_telemetry:
        for tf in telemetry_files:
            telemetry_path = os.path.join(telemetry_dir, tf)
            data = parse_sensor_data(telemetry_path)
            if data:
                duration = max(duration, data[-1].get('time', 0))
    
    return {
        'id': folder_name,
        'date': folder_name,
        'duration': duration,
        'status': 'success' if has_telemetry and videos else 'partial' if has_telemetry or videos else 'pending',
        'cameras': len(videos),
        'videos': videos,
        'telemetryFiles': telemetry_files,
        'hasTelemetry': has_telemetry
    }


# ============================================================================
# SERVE REACT APP
# ============================================================================

@app.route('/', defaults={'path': ''})
@app.route('/<path:path>')
def serve(path):
    if path.startswith('api/') or path.startswith('videos/'):
        abort(404)
    if path != "" and os.path.exists(os.path.join(app.static_folder, path)):
        return send_from_directory(app.static_folder, path)
    else:
        return send_from_directory(app.static_folder, 'index.html')


# ============================================================================
# FLIGHT MANAGEMENT API
# ============================================================================

@app.route('/api/flights', methods=['GET'])
def get_flights():
    """Get list of all flights"""
    flights = []
    
    if os.path.exists(DATA_DIR):
        for folder_name in sorted(os.listdir(DATA_DIR), reverse=True):
            folder_path = os.path.join(DATA_DIR, folder_name)
            if os.path.isdir(folder_path):
                # Check if it's a valid date folder
                try:
                    datetime.strptime(folder_name, '%Y-%m-%d')
                    flights.append(get_flight_info(folder_path))
                except ValueError:
                    continue
    
    return jsonify({'flights': flights})


@app.route('/api/flights/<flight_id>', methods=['GET'])
def get_flight(flight_id):
    """Get details for a specific flight"""
    folder_path = os.path.join(DATA_DIR, flight_id)
    
    if not os.path.exists(folder_path):
        return jsonify({'error': 'Flight not found'}), 404
    
    return jsonify(get_flight_info(folder_path))


# ============================================================================
# VIDEO SERVING API
# ============================================================================


@app.route('/api/flights/<flight_id>/videos', methods=['GET'])
def list_videos(flight_id):
    """List all videos for a flight"""
    videos_dir = os.path.join(DATA_DIR, flight_id, 'videos')
    
    if not os.path.exists(videos_dir):
        return jsonify({'videos': []})
    
    videos = [f for f in os.listdir(videos_dir) 
              if allowed_file(f, ALLOWED_VIDEO_EXTENSIONS)]
    
    return jsonify({
        'videos': videos,
        'urls': [f'/api/flights/{flight_id}/videos/{v}' for v in videos]
    })


@app.route('/api/flights/<flight_id>/videos/<filename>', methods=['GET'])
def serve_video(flight_id, filename):
    """Serve a video file"""
    videos_dir = os.path.join(DATA_DIR, flight_id, 'videos')
    return send_from_directory(videos_dir, filename)


# ============================================================================
# TELEMETRY DATA API
# ============================================================================

@app.route('/api/flights/<flight_id>/telemetry', methods=['GET'])
def get_telemetry(flight_id):
    """Get parsed telemetry data for a flight"""
    telemetry_dir = os.path.join(DATA_DIR, flight_id, 'telemetry')
    
    if not os.path.exists(telemetry_dir):
        return jsonify({'flight_id': flight_id, 'data': []})
    
    # Combine all telemetry files
    all_data = []
    for filename in os.listdir(telemetry_dir):
        if allowed_file(filename, ALLOWED_DATA_EXTENSIONS):
            file_path = os.path.join(telemetry_dir, filename)
            data = parse_sensor_data(file_path)
            all_data.extend(data)
    
    # Sort by time
    all_data.sort(key=lambda x: x.get('time', 0))
    
    return jsonify({
        'flight_id': flight_id,
        'data': all_data
    })


@app.route('/api/flights/<flight_id>/telemetry/raw', methods=['GET'])
def get_raw_telemetry(flight_id):
    """Get raw telemetry file contents"""
    telemetry_dir = os.path.join(DATA_DIR, flight_id, 'telemetry')
    
    if not os.path.exists(telemetry_dir):
        return jsonify({'files': []})
    
    files_data = []
    for filename in os.listdir(telemetry_dir):
        if allowed_file(filename, ALLOWED_DATA_EXTENSIONS):
            file_path = os.path.join(telemetry_dir, filename)
            with open(file_path, 'r') as f:
                content = f.read()
            files_data.append({
                'filename': filename,
                'content': content
            })
    
    return jsonify({'files': files_data})

# ============================================================================
# HEALTH CHECK
# ============================================================================

@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    return jsonify({
        'status': 'healthy',
        'data_directory': DATA_DIR,
        'timestamp': datetime.now().isoformat()
    })


if __name__ == '__main__':
    print(f"Starting server...")
    print(f"Data directory: {DATA_DIR}")
    print(f"Static folder: {app.static_folder}")
    app.run(debug=True, host='0.0.0.0', port=5000)
