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

    if (newData.gpsLock) {
        document.getElementById('gps_status').innerText = newData.gpsLock;
    }

    if (newData.gpsLat) {
        document.getElementById('lat').innerText = newData.gpsLat.toFixed(6);
    }

    if (newData.gpsLat_N) {
        document.getElementById('lato').innerText = newData.gpsLat_N;
    }

    if (newData.gpsLon) {
        document.getElementById('lon').innerText = newData.gpsLon.toFixed(6);
    }

    if (newData.gpsLon_W) {
        document.getElementById('lono').innerText = newData.gpsLon_W;
    }

    if (newData.horizontalVelocity) {
        document.getElementById('gps_speed').innerText = newData.horizontalVelocity.toFixed(6);
    }

    if (newData.course) {
        document.getElementById('course').innerText = newData.course.toFixed(6);
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

});

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
