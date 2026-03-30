import { AtmosphericSample } from "../models/atmospheric-sample";

const randomInRange = (min: number, max: number) => Math.random() * (max - min) + min;
const randomBool = (trueChance = 0.9) => Math.random() < trueChance;

// altitudine
let testAlt = 0;
let altDirection = 1; // se è 1 sale, -1 scende

// gps
let currentLat = 43.35000;
let currentLong = 10.10000;

// Velocità base di spostamento x gps
let latSpeed = 0.00002; 
let longSpeed = 0.00004;

export const makeSample = (): AtmosphericSample => {
    testAlt += 15 * altDirection;

    if (testAlt >= 1200) {
        testAlt = 1200;
        altDirection = -1;
    } else if (testAlt <= 0) {
        testAlt = 0;
        altDirection = 1;
    }

    latSpeed += randomInRange(-0.000015, 0.000015);
    longSpeed += randomInRange(-0.000015, 0.000015);
    
    // simulo il freno
    latSpeed *= 0.98;
    longSpeed *= 0.98;
    
    currentLat += latSpeed;
    currentLong += longSpeed;

    return {
        ts: Date.now(),
        
        roll: randomInRange(-45, 45),
        pitch: randomInRange(-45, 45),
        yaw: randomInRange(0, 360),
        status: randomBool(0.95),

        alt: testAlt, 

        vvel: randomInRange(-40, 40),
        hvel: randomInRange(0, 80),
        
        
        lat: currentLat,
        long: currentLong,
        
        gps: randomBool(0.85),
        temp: randomInRange(-5, 35),
        pres: randomInRange(950, 1030),
        rh: randomInRange(10, 95),
        accelX: randomInRange(-1, 1),
        accelY: randomInRange(-1, 1),
        accelZ: randomInRange(-1, 1),
    };
};

