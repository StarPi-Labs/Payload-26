// Import module
import { TelemetryAPI } from './telemetry.js'

// start the web socket
TelemetryAPI.start()

const unsubscribeData = TelemetryAPI.subscribeToData((newData) => {
    if (newData.time) {
        //const dateStr = new Date(newData.time * 1000).toISOString();
        const dateStr = new Date(newData.time * 1000).toLocaleTimeString();
        document.getElementById('utc').innerText = dateStr;
    }

    if (newData.gpsAlt) {
        document.getElementById('gps_alt').innerText = newData.gpsAlt.toFixed(3);
    }

    if (newData.gpsLock) {
        document.getElementById('gps_status').innerText = newData.gpsLock;
    }

    if (newData.gpsLat) {
        document.getElementById('lat').innerText = newData.gpsLat.toFixed(3);
    }

    if (newData.gpsLat_N) {
        document.getElementById('lato').innerText = newData.gpsLat_N;
    }

    if (newData.gpsLon) {
        document.getElementById('lon').innerText = newData.gpsLon.toFixed(3);
    }

    if (newData.gpsLon_W) {
        document.getElementById('lono').innerText = newData.gpsLon_W;
    }

    if (newData.horizontalVelocity) {
        document.getElementById('gps_speed').innerText = newData.horizontalVelocity.toFixed(3);
    }

    if (newData.course) {
        document.getElementById('course').innerText = newData.course.toFixed(3);
    }

    if (newData.bus_volt) {
        document.getElementById('bus_voltage').innerText = newData.bus_volt.toFixed(3);
    }

    if (newData.shunt_mVolts) {
        document.getElementById('shunt_voltage').innerText = newData.shunt_mVolts.toFixed(3);
    }

    if (newData.current) {
        document.getElementById('current').innerText = newData.current.toFixed(3);
    }

    if (newData.acceleration) {
        document.getElementById('accel').innerText = newData.acceleration.toFixed(3);
        window.updateRocketAttitude(newData.accelerationX, newData.accelerationY, newData.accelerationZ);
    }

    if (newData.imu_temp) {
        document.getElementById('imu_temp').innerText = newData.imu_temp.toFixed(3);
    }

    if (newData.accelerationX) {
        document.getElementById('accelx').innerText = newData.accelerationX.toFixed(3);
    }

    if (newData.accelerationY) {
        document.getElementById('accely').innerText = newData.accelerationY.toFixed(3);
    }

    if (newData.accelerationZ) {
        document.getElementById('accelz').innerText = newData.accelerationZ.toFixed(3);
    }

    if (newData.roll) {
        document.getElementById('roll').innerText = newData.roll.toFixed(3);
    }

    if (newData.pitch) {
        document.getElementById('pitch').innerText = newData.pitch.toFixed(3);
    }

    if (newData.yaw) {
        document.getElementById('yaw').innerText = newData.yaw.toFixed(3);
    }


});



window.updateRocketAttitude = function(ax, ay, az) {
    // 1. Convert acceleration to Pitch and Roll in DEGREES
    const radToDeg = 180 / Math.PI;
    const pitch = Math.atan2(ay, Math.sqrt(ax * ax + az * az)) * radToDeg;
    const roll = Math.atan2(-ax, az) * radToDeg * 0.8; 

    const rocket = document.getElementById('css-rocket');

    if (rocket) {
        rocket.style.transform = `rotateX(${pitch}deg) rotateZ(${roll}deg)`;
    }
};

const unsubscribeStatus = TelemetryAPI.subscribeToStatus((status) => {
    document.getElementById('status').innerText = `> LINK_STATE: ${status}`;
});

// Call this to unsubscribe
//unsubscribeData();

/*
 // e.g. SOLID Usage (gemini's recommendation)
import { createSignal, onMount, onCleanup } from "solid-js";
import { TelemetryAPI } from "./telemetry.js";

export function Dashboard() {
    const [data, setData] = createSignal(TelemetryAPI.getCurrentData());

    onMount(() => {
        // Start the connection
        TelemetryAPI.start();

        // Pass the SolidJS setter 
        const unsubscribe = TelemetryAPI.subscribeToData((newData) => {
            setData(newData);
        });

        // Clean up when the component closes
        onCleanup(() => unsubscribe());
    });

    // Render UI
}
 */
