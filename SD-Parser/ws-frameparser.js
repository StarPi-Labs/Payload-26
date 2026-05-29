export const TelemetryAPI = (function() {
    let socket;
    let latestData = null;
    let status = "DISCONNECTED";
    
    const listeners = new Set();
    const statusListeners = new Set();

    const SCHEMA = [
        { name: 'time',               type: 'f64' },    // 8 bytes
        { name: 'sys-timestamp_ms',   type: 'i32' },    // 4 byte
        { name: 'frame-status',       type: 'c'   },    // 1 byte
        { name: 'altitude',           type: 'f32' },    // 4 bytes
        { name: 'velocity',           type: 'f32' },
        { name: 'horizontalVelocity', type: 'f32' },
        { name: 'acceleration',       type: 'f32' },
        { name: 'accelerationX',      type: 'f32' },
        { name: 'accelerationY',      type: 'f32' },
        { name: 'accelerationZ',      type: 'f32' },
        { name: 'imu_temp',           type: 'f32' },
        { name: 'temperature',        type: 'f32' },
        { name: 'pressure',           type: 'f32' },
        { name: 'humidity',           type: 'f32' },
        { name: 'gpsLat',             type: 'f32' },
        { name: 'gpsLat_N',           type: 'c'   },
        { name: 'gpsLon',             type: 'f32' },
        { name: 'gpsLon_W',           type: 'c'   },
        { name: 'gpsLock',            type: 'c'   },
        { name: 'gpsAlt',             type: 'f32' },
        { name: 'pitch',              type: 'f32' },
        { name: 'roll',               type: 'f32' },
        { name: 'yaw',                type: 'f32' },
        { name: 'shunt_mVolts',       type: 'f32' },
        { name: 'bus_volt',           type: 'f32' },
        { name: 'current',            type: 'f32' },
    ];

    function connect() {
        if (socket) socket.close();
        socket = new WebSocket(`ws://127.0.0.1:5000/raw_ws?dev=debug`);

        socket.onopen = () => updateStatus("ACTIVE");

        socket.onmessage = async (event) => {
            const buffer = await event.data.arrayBuffer();
            const view = new DataView(buffer);
            const frame_info = view.getUint32(0, true);
            let offset = 4;
            const currentData = {};
        
            for (let i = 0; i < SCHEMA.length; i++) {
                if ((frame_info & (1 << i)) !== 0) {
                    const field = SCHEMA[i];
                    if (field.type === 'f64') {
                        currentData[field.name] = view.getFloat64(offset, true);
                        offset += 8;

                    } else if (field.type === 'f32') {
                        currentData[field.name] = view.getFloat32(offset, true);
                        offset += 4;

                    } else if (field.type === 'i32') {
                        currentData[field.name] = view.getUint32(offset, true);
                        offset += 4;

                    } else if (field.type === 'c') {
                        currentData[field.name] = String.fromCharCode(view.getUint8(offset));
                        offset += 1;
                    }
                }
            }

            latestData = currentData;
            listeners.forEach(callback => callback(latestData));
        };

        socket.onclose = () => {
            updateStatus("RECONNECTING...");
            setTimeout(connect, 2000);
        };
    }

    function updateStatus(newStatus) {
        status = newStatus;
        statusListeners.forEach(callback => callback(status));
    }

    // API Interfaces
    return {
        start: connect,
        
        // Returns Data and State 
        getCurrentData: () => latestData,
        getStatus: () => status,

        // Allows to add callbacks for data
        subscribeToData: (callback) => {
            listeners.add(callback);
            // Return an unsubscribe function (Crucial for frontend frameworks!)
            return () => listeners.delete(callback);
        },
        
        // Allows to add callbacks for status
        subscribeToStatus: (callback) => {
            statusListeners.add(callback);
            return () => statusListeners.delete(callback);
        }
    };
})();

