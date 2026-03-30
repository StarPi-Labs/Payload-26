import { Component, onMount, createEffect, onCleanup } from "solid-js";
import L from "leaflet";
import "leaflet/dist/leaflet.css";
import TelemetryCard from "./base/TelemetryCard";
import {RocketMapCardProps} from "../models/ui/rocket-map-card-props"

// Fix per le icone di default di Leaflet
delete (L.Icon.Default.prototype as any)._getIconUrl;
L.Icon.Default.mergeOptions({
  iconRetinaUrl: 'https://unpkg.com/leaflet@1.9.4/dist/images/marker-icon-2x.png',
  iconUrl: 'https://unpkg.com/leaflet@1.9.4/dist/images/marker-icon.png',
  shadowUrl: 'https://unpkg.com/leaflet@1.9.4/dist/images/marker-shadow.png',
});


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

    // il punto iniziale  rosso
    const redIcon = new L.Icon({
      iconUrl: 'https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-2x-red.png',
      shadowUrl: 'https://unpkg.com/leaflet@1.9.4/dist/images/marker-shadow.png',
      iconSize: [25, 41],
      iconAnchor: [12, 41],
      popupAnchor: [1, -34],
      shadowSize: [41, 41]
    });
    L.marker([props.latitude, props.longitude], { icon: redIcon })
      .addTo(map)

    // percorso 
    trajectory = L.polyline([[props.latitude, props.longitude]], {
      color: '#ef4444', 
      weight: 3,       
      opacity: 0.8,
    }).addTo(map);

    //posizione attuale
    currentMarker = L.marker([props.latitude, props.longitude]).addTo(map);
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