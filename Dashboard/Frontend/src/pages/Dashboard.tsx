import { Component, createSignal, onCleanup } from "solid-js";

import { AtmosphericSample } from "../models/atmospheric-sample";
import { makeSample } from "../utils/mock-telemetry";

import AttitudeCard from "../components/AttitudeCard";
import AtmosphereCard from "../components/AtmosphereCard";
import NavigationCard from "../components/base/NavigationCard";
import VelocityGraphCard from "../components/VelocityGraphCard";
import AltitudeGraphCard from "../components/AltitudeGraphCard";
import AccellerationGraphCard from "../components/AccellerationGraphCard";
import AltitudeTracker from "../components/AltitudeTracker";
import VideoPlayer from "../components/VideoPlayer";
import RocketMapCard from "../components/RocketMapCard"

const Dashboard: Component = () => {
    
    // il codice per i test (makeSample) è in utils/mock-telemetry

    const [sample, setSample] = createSignal<AtmosphericSample>(makeSample());

    const interval = setInterval(() => {
        setSample(makeSample());
    }, 100);

    onCleanup(() => clearInterval(interval));

    const timestampLabel = () => {
        const value = sample().ts;
        return new Date(value).toLocaleTimeString();
    };

    return (
        <div class="space-y-6">
            <div class="flex flex-col gap-2">
                <h1 class="text-2xl font-semibold">Dashboard</h1>
            </div>

            {/* DATI METRICI */}
            <div class="grid grid-cols-1 xl:grid-cols-3 gap-4">
                <AttitudeCard
                    roll={sample().roll}
                    pitch={sample().pitch}
                    yaw={sample().yaw}
                    status={sample().status}
                    timestampLabel={timestampLabel()}
                />
                <AtmosphereCard
                    temperature={sample().temp}
                    pressure={sample().pres}
                    humidity={sample().rh}
                />
                <NavigationCard
                    altitude={sample().alt}
                    verticalVelocity={sample().vvel}
                    horizontalVelocity={sample().hvel}
                    latitude={sample().lat}
                    longitude={sample().long}
                    gpsFix={sample().gps}
                />
            </div>

            <div class="grid grid-cols-1 lg:grid-cols-4 gap-4">
                {/* VIDEO PLAYER */}
                <div class="lg:col-span-2 h-full">
                    <VideoPlayer
                        sources={[
                            { id: "cam1", label: "Front Camera", src: "assets/video/199582-910653711_medium.mp4" },
                            { id: "cam2", label: "Bottom Camera", src: "assets/video/854224-hd_1280_720_30fps.mp4" },
                            { id: "cam3", label: "Arm Camera", src: "assets/video/arm-test.mp4" }
                        ]}
                        objectFit="cover"
                    />
                </div>
                
                <div class="flex flex-col sm:flex-row gap-4 lg:col-span-2">
                    {/* SOTTO-COLONNA GRAFICI */}
                    <div class="flex flex-col gap-4 flex-1 w-full sm:w-0">
                        <VelocityGraphCard
                            time={sample().ts}
                            verticalVelocity={sample().vvel}
                            horizontalVelocity={sample().hvel}
                            class="w-full"
                        />

                        <AltitudeGraphCard
                            time={sample().ts}
                            altitude={sample().alt}
                            class="w-full"
                        />
                    </div>
                    
                    {/* MINI COLONNA TRACKER ALTITUDINE */}
                    <AltitudeTracker
                        currentAltitude={sample().alt}
                        targetAltitude={1200}
                        maxAltitude={1200}
                        class="w-full sm:w-28 shrink-0 h-[350px] sm:h-auto"
                    />

                </div>

            </div>

            {/* GRAFICO */}
            <div class="grid gap-4">
                <AccellerationGraphCard
                    time={sample().ts}
                    accelX={sample().accelX}
                    accelY={sample().accelY}
                    accelZ={sample().accelZ}
                    class="w-full"
                />
            </div>
            
            {/* MAPPA */}
            <RocketMapCard
                latitude={sample().lat}
                longitude={sample().long}
                gpsFix={sample().gps}
            />
        </div>

    );
};

export default Dashboard;
