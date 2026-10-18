/**
 * rocket-renderer.ts -- the three.js scene behind RocketView.
 *
 * A fixed camera looks horizontally at the rocket from the front (the side
 * away from the launch rail), on black. Only the rocket moves: it turns with
 * the payload's orientation, and its own acceleration pushes it off centre -
 * up while the motor burns, down while drag and gravity slow it, back to the
 * middle at rest or hanging steady under the parachute.
 *
 * Frames (Dashboard/Backend/attitude.py derives the data):
 *   body   payload CAD frame: +X front (away from the rail), +Y right, +Z nose
 *   pad    X towards the viewer, Y right, Z up - fixed by the payload's pose at
 *          the start of the recording, so it begins in the CAD front view
 *   three  x right, y up, z towards the viewer (the camera sits on +z)
 *   model  the model file's own axes, tied to body by noseAxis/frontAxis
 *
 * Nothing is drawn unless something changed (attitude, size, model), so a
 * paused dashboard costs no GPU time at all.
 *
 * This module is imported dynamically by RocketView, which keeps three.js out
 * of the dashboard's main bundle.
 */
import {
    Box3,
    BoxGeometry,
    CylinderGeometry,
    DirectionalLight,
    DoubleSide,
    ExtrudeGeometry,
    Group,
    HemisphereLight,
    LatheGeometry,
    Material,
    Matrix4,
    Mesh,
    MeshStandardMaterial,
    NeutralToneMapping,
    Object3D,
    PerspectiveCamera,
    PMREMGenerator,
    Quaternion,
    Scene,
    Shape,
    SRGBColorSpace,
    Texture,
    Vector2,
    Vector3,
    WebGLRenderer,
} from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { DRACO_GLTF_CONFIG, DRACOLoader } from "three/addons/loaders/DRACOLoader.js";
import { MeshoptDecoder } from "three/addons/libs/meshopt_decoder.module.js";
import { RoomEnvironment } from "three/addons/environments/RoomEnvironment.js";
import { RocketModelConfig } from "../models/ui/rocket-view-props";

const FOV_DEG = 20;
/** Share of the panel's smaller side the rocket's bounding sphere spans at rest;
 * the rest of the panel is room for it to be pushed around. */
const FILL = 0.78;
/** Acceleration (g) that pushes the rocket ~76 % of the way to the edge (tanh(1)). */
const ACCEL_REF_G = 3;
/** Movement towards/away from the camera, at most this share of the rocket's radius. */
const DEPTH_FRACTION = 0.5;
const G0 = 9.80665;
/** Exposure while the flight has no attitude data: the rocket stands dimmed. */
const DIMMED_EXPOSURE = 0.3;

const AXES: Record<string, [number, number, number]> = {
    "+X": [1, 0, 0], "-X": [-1, 0, 0],
    "+Y": [0, 1, 0], "-Y": [0, -1, 0],
    "+Z": [0, 0, 1], "-Z": [0, 0, -1],
};

/** pad -> three: three.x = pad.y, three.y = pad.z, three.z = pad.x */
const PAD_TO_THREE = new Quaternion().setFromRotationMatrix(new Matrix4().makeBasis(
    new Vector3(0, 0, 1),   // where pad X ends up
    new Vector3(1, 0, 0),   // pad Y
    new Vector3(0, 1, 0),   // pad Z
));

/** Rotation taking model coordinates to body coordinates, or null if the two
 * axes given are not a valid perpendicular pair. */
function modelToBody(noseAxis: string, frontAxis: string): Quaternion | null {
    const n = AXES[(noseAxis || "").trim().toUpperCase()];
    const f = AXES[(frontAxis || "").trim().toUpperCase()];
    if (!n || !f) return null;
    const nose = new Vector3(...n);
    const front = new Vector3(...f);
    if (Math.abs(nose.dot(front)) > 1e-6) return null;
    const right = new Vector3().crossVectors(nose, front);   // body Y = Z x X
    // makeBasis maps body X/Y/Z onto the model's front/right/nose: body -> model.
    const bodyToModel = new Quaternion().setFromRotationMatrix(new Matrix4().makeBasis(front, right, nose));
    return bodyToModel.invert();
}

/** Generic rocket, drawn until a real model is exported (Documentation/ROCKET_MODEL.md).
 * Built in model coordinates matching the default config: nose +Y, front +Z,
 * right +X. One orange fin and a blue stripe down the front make spin visible;
 * the rail buttons sit on the back. */
function buildGenericRocket(): Group {
    const rocket = new Group();
    const R = 0.06;
    const mat = (color: number, roughness = 0.45, metalness = 0.05) =>
        new MeshStandardMaterial({ color, roughness, metalness, side: DoubleSide });
    const bodyMat = mat(0xe8e8e8);
    const noseMat = mat(0xd8382b, 0.35);
    const finMat = mat(0x5f6673, 0.5, 0.1);      // light enough to read against black
    const markMat = mat(0xff8a1f, 0.4);
    const stripeMat = mat(0x2457d6, 0.4);
    const darkMat = mat(0x3c3c3c, 0.6, 0.3);

    const body = new Mesh(new CylinderGeometry(R, R, 1.25, 48), bodyMat);
    body.position.y = -0.075;                                  // y = -0.70 ... 0.55
    rocket.add(body);

    // Tangent ogive nose cone, 0.40 m.
    const L = 0.40;
    const rho = (R * R + L * L) / (2 * R);
    const profile: Vector2[] = [];
    for (let i = 0; i <= 32; i++) {
        const u = (i / 32) * L;                                // from the base up
        profile.push(new Vector2(Math.max(0, Math.sqrt(rho * rho - u * u) + R - rho), u));
    }
    const nose = new Mesh(new LatheGeometry(profile, 48), noseMat);
    nose.position.y = 0.55;
    rocket.add(nose);

    // Four clipped-delta fins between the rail buttons; the orange one is front-right.
    const fin = new Shape();
    fin.moveTo(0, 0);
    fin.lineTo(0, 0.22);
    fin.lineTo(0.12, 0.15);
    fin.lineTo(0.12, 0.05);
    fin.closePath();
    const finGeo = new ExtrudeGeometry(fin, { depth: 0.006, bevelEnabled: false });
    finGeo.translate(R - 0.002, -0.70, -0.003);
    for (let k = 0; k < 4; k++) {
        const m = new Mesh(finGeo, k === 0 ? markMat : finMat);
        m.rotation.y = -Math.PI / 4 + k * (Math.PI / 2);
        rocket.add(m);
    }

    const stripe = new Mesh(new BoxGeometry(0.018, 1.05, 0.003), stripeMat);
    stripe.position.set(0, -0.06, R - 0.0003);                 // front (+Z) face
    rocket.add(stripe);

    const buttonGeo = new CylinderGeometry(0.008, 0.008, 0.014, 16);
    buttonGeo.rotateX(Math.PI / 2);
    for (const y of [-0.45, 0.25]) {
        const b = new Mesh(buttonGeo, darkMat);
        b.position.set(0, y, -R - 0.006);                      // back (-Z) = rail side
        rocket.add(b);
    }

    const nozzle = new Mesh(new CylinderGeometry(0.025, 0.033, 0.05, 32), darkMat);
    nozzle.position.y = -0.725;
    rocket.add(nozzle);
    return rocket;
}

function disposeObject(root: Object3D) {
    root.traverse((obj) => {
        const mesh = obj as Mesh;
        if (mesh.geometry) mesh.geometry.dispose();
        const mats = mesh.material
            ? (Array.isArray(mesh.material) ? mesh.material : [mesh.material]) as Material[]
            : [];
        for (const m of mats) {
            for (const value of Object.values(m)) {
                if (value instanceof Texture) value.dispose();
            }
            m.dispose();
        }
    });
}

export class RocketRenderer {
    private renderer: WebGLRenderer;
    private scene = new Scene();
    private camera = new PerspectiveCamera(FOV_DEG, 1, 0.01, 100);
    private motion = new Group();       // offset by acceleration
    private attitude = new Group();     // body -> pad -> three
    private align = new Group();        // model -> body, pivot at the origin
    private model: Object3D | null = null;
    private loadToken = 0;
    private radius = 1;                 // bounding sphere of the rocket about its pivot
    private roomX = 0;                  // free space to the panel edges at rest, world units
    private roomY = 0;
    private width = 1;
    private height = 1;
    private frame = 0;
    private disposed = false;
    private quat = new Quaternion();
    private accel: [number, number, number] = [0, 0, 0];   // pad frame, m/s^2
    private hasAttitude = false;
    private gltf = new GLTFLoader();
    private draco = new DRACOLoader();

    constructor(canvas: HTMLCanvasElement) {
        this.renderer = new WebGLRenderer({ canvas, antialias: true, powerPreference: "low-power" });
        this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
        this.renderer.outputColorSpace = SRGBColorSpace;
        this.renderer.toneMapping = NeutralToneMapping;
        this.renderer.setClearColor(0x000000, 1);

        // CAD exports are physically based and often metallic; without an
        // environment to reflect, metal renders black against a black background.
        const pmrem = new PMREMGenerator(this.renderer);
        this.scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
        this.scene.environmentIntensity = 0.6;
        pmrem.dispose();

        const key = new DirectionalLight(0xffffff, 2.2);
        key.position.set(3, 4, 5);
        const rim = new DirectionalLight(0x9ab8ff, 1.6);       // outlines it against the black
        rim.position.set(-4, 2, -5);
        this.scene.add(new HemisphereLight(0xffffff, 0x202020, 0.5), key, rim);

        // SolidWorks can Draco-compress its export. The glTF-only decoder that
        // ships with three is enough; Vite bundles it as an asset.
        this.draco.setDecoderPath(DRACO_GLTF_CONFIG);
        this.gltf.setDRACOLoader(this.draco);
        this.gltf.setMeshoptDecoder(MeshoptDecoder);

        this.attitude.add(this.align);
        this.motion.add(this.attitude);
        this.scene.add(this.motion);
        this.camera.position.set(0, 0, 10);
        this.camera.lookAt(0, 0, 0);
        this.applyPose();
    }

    /** Draw this model (null = the generic rocket). Superseded loads are dropped. */
    async setModel(config: RocketModelConfig | null): Promise<void> {
        const token = ++this.loadToken;
        let object: Object3D | null = null;
        let align = modelToBody(config?.noseAxis ?? "+Y", config?.frontAxis ?? "+Z");
        if (config && !align) {
            console.warn(`rocket view: noseAxis ${config.noseAxis} / frontAxis ${config.frontAxis} ` +
                "must be two perpendicular axes such as \"+Y\" and \"+Z\" - using the defaults");
            align = modelToBody("+Y", "+Z");
        }
        if (config?.url) {
            try {
                object = (await this.gltf.loadAsync(config.url)).scene;
            } catch (e) {
                console.warn(`rocket view: could not load ${config.url}, drawing a generic rocket`, e);
            }
        }
        if (token !== this.loadToken || this.disposed) {
            if (object) disposeObject(object);
            return;
        }
        if (!object) {
            object = buildGenericRocket();
            align = modelToBody("+Y", "+Z");
        }

        if (this.model) {
            this.align.remove(this.model);
            disposeObject(this.model);
        }
        this.model = object;
        this.align.add(object);
        this.align.quaternion.copy(align!);

        // Turn about the configured pivot, else the middle of the bounding box.
        // Measured in body coordinates, detached from the current pose.
        this.attitude.remove(this.align);
        this.align.position.set(0, 0, 0);
        this.align.updateMatrixWorld(true);
        const box = new Box3().setFromObject(this.align);
        const pivot = config?.pivot;
        const pivotBody = Array.isArray(pivot) && pivot.length === 3
            ? new Vector3(pivot[0], pivot[1], pivot[2]).applyQuaternion(this.align.quaternion)
            : box.getCenter(new Vector3());
        this.align.position.copy(pivotBody).negate();
        box.translate(this.align.position);
        this.attitude.add(this.align);
        let r = 0;
        for (const x of [box.min.x, box.max.x])
            for (const y of [box.min.y, box.max.y])
                for (const z of [box.min.z, box.max.z])
                    r = Math.max(r, Math.hypot(x, y, z));
        this.radius = r > 0 ? r : 1;
        this.fitCamera();
        this.applyPose();
    }

    /** Orientation quaternion body -> pad (w, x, y, z), or null when unknown. */
    setAttitude(q: [number, number, number, number] | null) {
        const known = !!q && Math.hypot(q[0], q[1], q[2], q[3]) > 1e-6;
        if (known) {
            this.quat.set(q![1], q![2], q![3], q![0]).normalize();
        } else {
            this.quat.identity();
        }
        if (known !== this.hasAttitude) {
            this.hasAttitude = known;
            this.renderer.toneMappingExposure = known ? 1 : DIMMED_EXPOSURE;
        }
        this.applyPose();
    }

    /** The payload's acceleration in the pad frame, gravity removed, m/s^2. */
    setAcceleration(x: number, y: number, z: number) {
        this.accel = [x || 0, y || 0, z || 0];
        this.applyPose();
    }

    resize(width: number, height: number) {
        if (width <= 0 || height <= 0) return;
        this.width = width;
        this.height = height;
        this.renderer.setSize(width, height, false);
        this.fitCamera();
        this.applyPose();
    }

    dispose() {
        this.disposed = true;
        if (this.frame) cancelAnimationFrame(this.frame);
        if (this.model) disposeObject(this.model);
        this.scene.environment?.dispose();
        this.draco.dispose();
        this.renderer.dispose();
    }

    private fitCamera() {
        const aspect = this.width / this.height;
        const tanHalf = Math.tan((FOV_DEG * Math.PI) / 360);
        // The sphere spans FILL of the smaller side; the camera stays put after this.
        const dist = this.radius / (FILL * tanHalf * Math.min(1, aspect));
        this.camera.aspect = aspect;
        this.camera.position.set(0, 0, dist);
        this.camera.near = Math.max(0.001, dist - 3 * this.radius);
        this.camera.far = dist + 3 * this.radius;
        this.camera.updateProjectionMatrix();
        const halfH = dist * tanHalf;
        this.roomY = Math.max(0, halfH - this.radius);
        this.roomX = Math.max(0, halfH * aspect - this.radius);
    }

    private applyPose() {
        this.attitude.quaternion.copy(PAD_TO_THREE).multiply(this.quat);

        // Same scale in every direction for small pushes, so the rocket moves
        // the way it accelerated; each axis saturates softly at the panel edge.
        const [ax, ay, az] = this.hasAttitude ? this.accel.map((v) => v / G0) : [0, 0, 0];
        const perG = Math.max(this.roomX, this.roomY) / ACCEL_REF_G;
        const soft = (room: number, v: number) => (room > 0 ? room * Math.tanh((perG * v) / room) : 0);
        this.motion.position.set(
            soft(this.roomX, ay),                                          // pad Y: right
            soft(this.roomY, az),                                          // pad Z: up
            DEPTH_FRACTION * this.radius * Math.tanh(ax / ACCEL_REF_G),   // pad X: towards the viewer
        );
        this.requestRender();
    }

    private requestRender() {
        if (this.frame || this.disposed) return;
        this.frame = requestAnimationFrame(() => {
            this.frame = 0;
            if (!this.disposed) this.renderer.render(this.scene, this.camera);
        });
    }
}
