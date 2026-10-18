# Rocket model for the Dashboard's attitude view

The Dashboard shows a window next to the camera view: a fixed front view of the
rocket on black. The rocket turns the way the payload turned, and its own
acceleration pushes it off centre. It moves up while the motor burns, down while
drag slows it, and back to the middle at rest or hanging steady under the
parachute. Until you give it a model, it draws a generic rocket (white body, red
nose, an orange fin and a blue stripe down the front, so you can see it spin).

Attitude is only computed for flights from **18 October 2026** on, the first data
logged with the IMU in the CAD frame (`CONFIG_IMU_REMAP_TO_CAD`). Older flights
show the rocket standing still and dimmed.

## 1. Prepare the assembly in SolidWorks

- **Only the outside.** The view never shows internals. Hide or suppress
  electronics, fasteners, parachutes, harnesses and threaded rods, in a
  configuration or display state made for this. Alternatively use
  *File → Save As → Part (\*.sldprt)* and choose **Exterior components**, which
  drops anything that cannot be seen from outside. Fewer triangles means a
  smaller file and a smoother view.
- **Colours.** Appearances are exported as materials. Give the fins, the nose and
  any markings distinct colours: on a rocket that looks the same all round, a
  coloured fin is what lets you see it roll.
- **Orientation.** The defaults expect the rocket **standing upright, nose
  along +Y**, with the **Front view (Ctrl+1) looking at the side away from the
  launch rail** (the rail buttons point to −Z). If your assembly is built
  differently, note which axis points to the nose and which one points away
  from the rail. You will put them in `rocket.json` (section 4).
- **Detail.** Curved faces dominate the triangle count. If the export is heavy,
  lower *Tools → Options → Document Properties → Image Quality* before
  exporting, or simplify in step 3.

## 2. Export as GLB

*File → Save As* → **Extended Reality Binary (\*.glb)** → *Save*.

- Available since SOLIDWORKS 2019. If the type is missing, enable
  **SOLIDWORKS XR Exporter** under *Tools → Add-ins* (on older versions it is a
  separate download from the SOLIDWORKS Customer Portal, *Downloads and
  Updates*).
- Export options: up to 2024 behind the **Options…** button in the Save As
  dialog; from 2025 under *Tools → Options → System Options → Export*. Leave
  views, cameras, lights, motion studies and exploded views **off**: the
  Dashboard uses only geometry and materials.
- **Draco compression** may be on or off. The viewer decodes both, but leaving it
  off and compressing in step 3 gives smaller files.
- Choose `.glb`, not `.gltf`: one self-contained file.

No XR exporter on your licence? Export *STEP AP214*, open it in **FreeCAD**
(free), select the bodies and use *File → Export → glTF (\*.glb)*. As a last
resort, export STL and colour it in **Blender** before exporting glTF, since STL
carries no colours.

## 3. Make it light for the browser

[glTF Transform](https://gltf-transform.dev) (needs Node.js) inspects and
compresses the file:

```bash
npx @gltf-transform/cli inspect rocket.glb
```

```bash
npx @gltf-transform/cli optimize rocket.glb rocket-web.glb --compress meshopt --texture-compress webp --texture-size 1024 --simplify-error 0.001
```

- `--compress meshopt` shrinks geometry several times and decodes almost
  instantly in the browser.
- `--simplify-error 0.001` lets it remove triangles up to 0.1 % of the model's
  size: about 1.7 mm on a 1.7 m rocket, invisible in the view.
- Do **not** use `--texture-compress ktx2`: the viewer has no KTX2 decoder.
- Aim for a few MB and a few hundred thousand triangles at most (the `inspect`
  output lists both). For reference, the full `General_Assembly.glb` export
  (2.9 m rocket, 157 MB, ~2.2 million triangles) came out of this command at
  4.3 MB and ~560 000 triangles, and loads in a fraction of a second.

## 4. Put it where the Dashboard finds it

| Path in the data folder | Used for |
|---|---|
| `models/rocket.glb` | every flight |
| `<YYYY-MM-DD>/model/rocket.glb` | that flight only (a different rocket) |

Locally the data folder is `Dashboard/Backend/data/`; on the Docker server it is
whatever `PILOG_DATA_PATH` points to. No rebuild or restart is needed, just
reload the page. If a folder has several models, the first `.glb` wins, unless
`rocket.json` names one.

Next to the model, an optional `rocket.json` says how the model is oriented:

```json
{
  "model": "rocket-web.glb",
  "noseAxis": "+Y",
  "frontAxis": "+Z",
  "pivot": [0, 0.85, 0]
}
```

| Key | Meaning | Default |
|---|---|---|
| `model` | which file in the folder to use | first `.glb` |
| `noseAxis` | model axis pointing to the nose: `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z` | `+Y` |
| `frontAxis` | model axis pointing away from the rail; must be perpendicular to `noseAxis` | `+Z` |
| `pivot` | point the rocket turns about, in model units (metres in a SolidWorks GLB), e.g. the centre of gravity | middle of the bounding box |

Example: a rocket modelled lying along +X, with the rail buttons on −Y, needs
`"noseAxis": "+X", "frontAxis": "+Y"`.

## 5. Check it

Open a flight from 18 Oct 2026 or later and scrub to a moment where the rocket
stands still on the pad. You should see the nose up and the side facing you
should be the side away from the rail. If it lies down, faces sideways or shows
its back, fix `noseAxis` / `frontAxis` and reload. If the model does not appear
at all (generic rocket instead), the browser console says why.

## What the motion shows, and its limits

- **Rotation** is the payload's orientation, integrated from the gyro and pulled
  back to gravity whenever the payload is still (`Dashboard/Backend/attitude.py`).
  The camera is fixed by the payload's pose while it waited before launch:
  standing on the pad, its front faces you. Heading is relative, because
  nothing on board measures it (no magnetometer).
- **Offset** is the acceleration in that fixed frame with gravity removed,
  smoothed over 80 ms. 3 g pushes the rocket about three quarters of the way to
  the edge, and stronger pushes approach the edge without leaving it.
- **ARMED logging is sparse**: 0.4 s every 10 s. Whatever turned the payload
  between two bursts is not in the data. The view snaps the tilt to gravity at
  each burst and keeps the heading. From BOOST on, logging is continuous.
- **Gyro range ±500 °/s.** A spin faster than that saturates, so the view
  under-rotates.
- **Accelerometer range ±2 g outside BOOST**, including COAST. Drag above 2 g
  right after burnout clips, so the downward push stops at 2 g.
