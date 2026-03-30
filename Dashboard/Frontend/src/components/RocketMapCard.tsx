import { Component, onMount, createEffect, onCleanup } from "solid-js";
import L from "leaflet";
import "leaflet/dist/leaflet.css";
import TelemetryCard from "./base/TelemetryCard";
import {RocketMapCardProps} from "../models/ui/rocket-map-card-props"

const RocketMapCard: Component<RocketMapCardProps> = (props) => {
  let mapRef: HTMLDivElement | undefined;
  
  let map: L.Map | undefined;
  let currentMarker: L.Marker | undefined;
  let trajectory: L.Polyline | undefined;

  onMount(() => {
    if (!mapRef) return;
    
    map = L.map(mapRef).setView([props.latitude, props.longitude], 14);

    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
      attribution: '&copy; OpenStreetMap contributors',
    }).addTo(map);

    
    // icona razzo (pos attuale)
    const rocketHtml = `
      <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 256 256" style="filter: drop-shadow(0px 2px 4px rgba(0,0,0,0.5)); transform: rotate(45deg);">
        <g style="stroke: none; stroke-width: 0; fill: none; fill-rule: nonzero; opacity: 1;" transform="translate(1.4065934065934016 1.4065934065934016) scale(2.81 2.81)">
          <path d="M 89.983 5.63 c -0.006 -0.267 -0.016 -0.534 -0.026 -0.802 c -0.011 -0.299 -0.02 -0.597 -0.036 -0.897 c -0.031 -0.602 -0.07 -1.207 -0.121 -1.814 c -0.081 -0.973 -0.854 -1.745 -1.827 -1.827 c -0.607 -0.051 -1.21 -0.089 -1.811 -0.121 c -0.305 -0.016 -0.607 -0.025 -0.909 -0.036 c -0.262 -0.009 -0.525 -0.02 -0.786 -0.025 c -0.437 -0.01 -0.871 -0.013 -1.304 -0.013 c -0.072 0 -0.145 0 -0.217 0.001 c -8.628 0.042 -16.548 2.16 -24.544 6.526 C 58.261 6.7 58.12 6.773 57.979 6.85 c -0.05 0.028 -0.099 0.052 -0.149 0.08 c -0.011 0.006 -0.02 0.016 -0.031 0.022 c -6.556 3.654 -13.101 8.811 -19.875 15.585 c -0.77 0.77 -1.523 1.55 -2.268 2.334 l -13.164 1.001 c -0.385 0.029 -0.753 0.169 -1.06 0.402 L 0.785 41.987 c -0.657 0.5 -0.94 1.352 -0.711 2.145 c 0.228 0.793 0.92 1.364 1.742 1.439 l 19.373 1.749 l 6.134 6.134 c -2.174 0.497 -4.389 1.715 -6.286 3.611 c -1.136 1.137 -2.048 2.411 -2.716 3.803 c -0.873 1.849 -2.79 6.61 -4.82 11.651 l -0.991 2.459 c -0.3 0.744 -0.127 1.595 0.441 2.162 c 0.382 0.383 0.894 0.586 1.415 0.586 c 0.251 0 0.505 -0.048 0.748 -0.146 l 2.547 -1.027 c 5 -2.014 9.723 -3.917 11.576 -4.79 c 1.38 -0.664 2.655 -1.576 3.79 -2.711 c 1.896 -1.896 3.113 -4.111 3.61 -6.285 l 5.952 5.952 l 1.749 19.372 c 0.074 0.822 0.646 1.514 1.439 1.742 c 0.183 0.053 0.369 0.078 0.553 0.078 c 0.614 0 1.207 -0.283 1.592 -0.789 l 15.711 -20.646 c 0.233 -0.307 0.373 -0.675 0.402 -1.06 l 0.971 -12.775 c 0.857 -0.811 1.706 -1.635 2.547 -2.475 c 6.779 -6.779 11.939 -13.327 15.594 -19.887 c 0.004 -0.007 0.01 -0.013 0.014 -0.02 c 0.018 -0.032 0.033 -0.063 0.051 -0.095 c 0.167 -0.301 0.326 -0.602 0.486 -0.904 c 4.207 -7.847 6.251 -15.635 6.295 -24.099 c 0.001 -0.083 0.001 -0.165 0.001 -0.248 C 89.996 6.488 89.993 6.06 89.983 5.63 z M 64.413 37.493 c -1.577 1.577 -3.675 2.447 -5.907 2.447 c -2.231 0 -4.329 -0.869 -5.907 -2.447 c -3.257 -3.258 -3.257 -8.557 0 -11.815 v 0 c 3.259 -3.257 8.559 -3.255 11.814 0 c 1.578 1.577 2.448 3.675 2.448 5.907 S 65.992 35.915 64.413 37.493 z" style="stroke: none; fill: #3b82f6; fill-rule: nonzero;" stroke-linecap="round"/>
        </g>
      </svg>
    `;

    const rocketIcon = L.divIcon({
      html: rocketHtml,
      className: "bg-transparent transition-transform duration-200", 
      iconSize: [32, 32],
      iconAnchor: [16, 16], 
    });

    // icona partenza
    const redIcon = new L.Icon({
      iconUrl: 'https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-2x-red.png',
      shadowUrl: 'https://unpkg.com/leaflet@1.9.4/dist/images/marker-shadow.png',
      iconSize: [25, 41],
      iconAnchor: [12, 41],
      popupAnchor: [1, -34],
      shadowSize: [41, 41]
    });

    L.marker([props.latitude, props.longitude], { icon: redIcon }).addTo(map)
    currentMarker = L.marker([props.latitude, props.longitude], { icon: rocketIcon }).addTo(map);

    // percorso 
    trajectory = L.polyline([[props.latitude, props.longitude]], {
      color: '#ef4444', 
      weight: 3,        
      opacity: 0.8,
    }).addTo(map);

  });
  


  createEffect(() => {
    const lat = props.latitude;
    const long = props.longitude;

    if (map && currentMarker && trajectory) {
        currentMarker.setLatLng([lat, long]);
        trajectory.addLatLng([lat, long]);
        
        if (props.gpsFix) {
            map.panTo([lat, long]);
        }
    
    }
  });

  onCleanup(() => {
    if (map) {
      map.remove();
      map = undefined;
    }
  });

  return (
    <TelemetryCard title="Rocket Location" subtitle="Live Trajectory & Tracking" class={props.class}>
      <div class="flex flex-col gap-3">
        <div 
          ref={mapRef} 
          class="w-full h-[500px] bg-base-300 rounded-lg overflow-hidden border border-base-content/10 relative z-10"
        >
          <div class="absolute inset-0 flex items-center justify-center text-sm text-base-content/50 bg-base-300">
            Initializing map...
          </div>
        </div>

        <div class="flex justify-between items-center text-xs font-medium border-t border-base-content/5 pt-3">
            <div class="flex gap-4">
                <p><span class="text-base-content/60">Lat:</span> <span class="font-mono">{props.latitude.toFixed(5)}</span></p>
                <p><span class="text-base-content/60">Long:</span> <span class="font-mono">{props.longitude.toFixed(5)}</span></p>
            </div>
            <span class={`badge ${props.gpsFix ? 'badge-success' : 'badge-error'} badge-sm`}>
                {props.gpsFix ? 'FIX' : 'NO FIX'}
            </span>
        </div>
      </div>
    </TelemetryCard>
  );
};

export default RocketMapCard;