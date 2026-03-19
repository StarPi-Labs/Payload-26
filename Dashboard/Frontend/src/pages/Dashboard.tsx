import { Component, createSignal, onCleanup } from "solid-js";
import AttitudeCard from "../components/AttitudeCard";
import AtmosphereCard from "../components/AtmosphereCard";
import NavigationCard from "../components/NavigationCard";
import { AtmosphericSample } from "../models/atmospheric-sample";
import VelocityGraphCard from "../components/VelocityGraphCard";
import AltitudeGraphCard from "../components/AltitudeGraphCard";
import VideoPlayer from "../components/VideoPlayer";
import AccellerationGraphCard from "../components/AccellerationGraphCard";
import AltitudeTracker from "../components/AltitudeTracker";

const Dashboard: Component = () => {
    const randomInRange = (min: number, max: number) => Math.random() * (max - min) + min;
    const randomBool = (trueChance = 0.9) => Math.random() < trueChance;

    let testAlt = 0;
    let altDirection = 1; // 1 sale, -1 scende

    const makeSample = (): AtmosphericSample => {

        // Semplice incremento/decremento
        testAlt += 15 * altDirection;

        // Rimbalza tra 0 e 1200
        if (testAlt >= 1200) {
            testAlt = 1200;
            altDirection = -1;
        } else if (testAlt <= 0) {
            testAlt = 0;
            altDirection = 1;
        }

        return {
            ts: Date.now(),
            roll: randomInRange(-45, 45),
            pitch: randomInRange(-45, 45),
            yaw: randomInRange(0, 360),
            status: randomBool(0.95),

            alt: testAlt, // Usiamo il nostro contatore che va su e giù

            // Tutto il resto è tornato puramente randomico come lo avevi scritto tu:
            vvel: randomInRange(-40, 40),
            hvel: randomInRange(0, 80),
            lat: randomInRange(43.30, 43.45),
            long: randomInRange(10.05, 10.20),
            gps: randomBool(0.85),
            temp: randomInRange(-5, 35),
            pres: randomInRange(950, 1030),
            rh: randomInRange(10, 95),
            accelX: randomInRange(-1, 1),
            accelY: randomInRange(-1, 1),
            accelZ: randomInRange(-1, 1),
        };
    };

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

                <div class="lg:col-span-2 h-full">
                    <VideoPlayer
                        sources={[
                            { id: "cam1", label: "Front Camera", src: "/video/199582-910653711_medium.mp4" },
                            { id: "cam2", label: "Bottom Camera", src: "video/854224-hd_1280_720_30fps.mp4" },
                            { id: "cam3", label: "Arm Camera", src: "/video/arm-test.mp4" }
                        ]}
                        objectFit="cover"
                    />
                </div>

                <div class="flex flex-col sm:flex-row gap-4 lg:col-span-2">

                    {/* SOTTO-COLONNA GRAFICI (flex-1 gli fa prendere tutto lo spazio rimasto) */}
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

                    {/* IL RAZZO (Largo fisso 112px o 128px, ma si stira in altezza) */}
                    <AltitudeTracker
                        currentAltitude={sample().alt}
                        targetAltitude={1200}
                        maxAltitude={1200}
                        class="w-full sm:w-28 shrink-0 h-[350px] sm:h-auto"
                    />

                </div>

            </div>
            <div class="grid gap-4">
                <AccellerationGraphCard
                    time={sample().ts}
                    accelX={sample().accelX}
                    accelY={sample().accelY}
                    accelZ={sample().accelZ}
                    class="w-full"
                />
            </div>
        </div>

    );
};

export default Dashboard;
