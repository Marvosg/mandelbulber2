# Mandelbulber MCP usability review

Reviewed against Mandelbulber 2.35-dev, GUI pid 39489, socket `~/mandelbulber/mandelbulber2-501.sock`, by driving only the `mandelbulber` MCP tools. Nine agent presets were saved and each was rendered as a 32-frame 320×180 draft orbit. The user's toolbar presets and `settings.fract` were not overwritten.

Priority is what blocked or misled a scene, not nits.

## Fixed in this pass

Verified against GUI pid 55511 (rebuilt `build/mandelbulber2.app`, restarted so the new binary is the running one) through the MCP tools, plus `mcp/scripts/e2e_polish.py`, `e2e_test.py`, `e2e_slice2.py`, and `test_paths.py` (all passed). User toolbar presets were not modified.

| Issue | Fix | Check |
| --- | --- | --- |
| 1, 14. Summaries listed idle formula slots; no way to clear or start clean | Summaries list formulas the objects tree draws. Idle non-empty slots are `stored_but_not_drawn`. `clear_formula` empties a slot and refuses while a node still draws it unless `force` is set. `new_scene` installs one Mandelbulb, the default camera, and an empty flight path, in one undo step, and does not write a file or change the Save target. | `new_scene` left Save on the previous file. A Mandelbox written into slot 2 showed up only as stored. `clear_formula` on the drawn slot was refused; clearing slot 2 removed it. |
| 2. Any name containing "color" was treated as a color | The bridge uses the parameter type. `iteration_fog_color_1_maxiter` is a number. One bad field no longer drops the rest of the batch; the result has `applied` and `errors`. | Setting that maxiter to 4 together with `iteration_fog_enable`, `power`, and an unknown name applied the three real fields and returned an error only for the unknown name. |
| 3. `update_keyframe` replaced the camera | `frames_per_keyframe` alone leaves camera, target, rotation, fov, and distance as stored. Passing those fields writes them on the key. Index alone still snapshots the live camera. A second animation edit is rejected while one is in progress or an animation is rendering. | After moving the eye, updating only the frame count left the stored camera at `[2, -4, 1.5]` and fov 40. |
| 4. `frame_scene` locked onto the bailout sphere | The scan tries shells at 100, 24, 8, and 3 and keeps the tightest shell that still finds a surface. A hit on the outer limit is treated as the escape sphere: the camera stays put and the result says `measurement_unreliable`. The Mandelbox scale hint is added only when a drawn formula is a box. | On the Kleinian agent preset the camera moved to distance 7.0 (measured radius 2.74), not ~318. The note did not mention `mandelbox_scale`. |
| 5. `load_preset` retargeted Save | Loading a preset leaves `systemData.lastSettingsFile` alone unless `set_save_target` is true. The result says whether the Save target changed. `load_settings` and `load_example` still retarget Save. | `load_preset` "Kleinian bubble hall" kept the previous Save path and `save_target_changed` was false. |
| 6. Primitives omitted radius; no inside-camera warning | The objects tree includes primitive kind, radius, and size. `update_object` accepts `radius`. A preview sets `camera_inside` when at least a quarter of the sample cells are inside, and the note says to pull the camera back. | A sphere added at radius 0.62 read back as `primitive: sphere`, and `update_object` radius 1.1 read back as 1.1. The inside-camera flag was not re-triggered with a long interior shot in this pass. |
| 7. `describe_formula` dumped the whole window and named the wrong size control | The default result is `start_here` (three to six controls, with a known-good value). `verbose: true` returns every control that formula shows, plus the shared slot controls. IFS `where` names `IFS_scale`. `power` is described as the exponent, not the Mandelbox size. Pseudo Kleinian warns about the bailout sphere. | Mandelbulb `start_here` had 4 controls and omitted the full dump. Verbose Kleinian returned 41 formula controls. IFS `start_here` led with `IFS_scale`. |
| 8. Search ranking and pasted fog descriptions | Formula and parameter search rank an exact name, then a prefix, then a substring, then a description. An empty `list_examples` returns a short sample from each collection and a `collections` list with counts. `background_color_*` is filed under effects. Fog tooltips that had been copied onto the wrong fields were rewritten, including `iteration_fog_color_1_maxiter`. | `list_formulas` query `mandelbulb` led with Mandelbulb. Empty `list_examples` sampled all 7 collections. Effects query `background_color` returned `background_color_1`, `_2`, and `_3`. |
| 9. Status text and the animation timer | A finished job's `message` is `Finished, total time: …` from the whole-job wall clock. A late per-frame "100% done" line cannot overwrite it. The preview note names the quality you asked for (`draft`, `better`, or `full`). A job that is running says Rendering rather than Waiting. | A better preview's note started "Better preview…" and its message was `Finished, total time: 0.2s`. A 4-frame flight reported `Finished, total time: 0.1s` against about half a second of wall clock, polling included. |
| 10. `inspect_preset` hid the card | The text includes the drawn formulas (with power and scale), the objects tree, which effects are on, and `thumbnail` as the toolbar image path. | "Classic power-8 bulb" returned power 8, an effects object, and a thumbnail path under `~/mandelbulber/thumbnails/`. |
| 11. `undo` did not say what it undid | The result has `undone` (or `nothing_to_undo`) plus `camera_before` and `camera_after`. Redo returns `redone`. The startup auto-refresh no longer stores a second identical snapshot above that floor. Copying a keyframe morph also copies its frame index list, which had crashed the next keyframe render. | One undo after a flight edit returned `undone: set_flight_path` and both cameras. `e2e_polish` restored power 9 and then stopped with "Nothing to undo". |
| 12. Orbit had no arc length | `set_flight_path` kind `orbit` takes `sweep` in degrees, default 360. A shorter arc does not repeat the first frame. | `sweep` 90 was accepted and echoed on a 4-frame orbit. |
| 13. Preview payloads were the whole cell grid | The default grid is `best` plus the top five ranked cells and `ranked_total`. `grid: full` returns every cell. | A better preview's grid mode was `best` with 5 ranked cells and no full `cells` array. |

Still open from "What worked": `look_around` with prefer `space` can rank sky outside the frame above an opening. That ranking was left as it was.

Startup moved the previous autosave aside so the recovery question would not block the new process. Those copies are `~/mandelbulber/.autosave.fract.held-during-mcp-restart` and the numbered copies `-2` through `-5`. The running app writes its own `.autosave.fract`.

## P1 — Fix these first

### 1. Preset and preview formulas are the slot list, not the tree

`list_presets`, `inspect_preset`, and every `render_preview` / `render_animation` summary report every slot that still holds a formula. After the objects tree was reduced to one node, every card still read as Mandelbulb + Mandelbulb + Mandelbox (or similar). The Kleinian summary was `Pseudo Kleinian + Mandelbulb + Mandelbox` even though only slot 1 is drawn.

There is no way to clear a slot. `set_formula` only overwrites. `delete_object` removes the node and leaves the slot formula behind. `list_formulas` query `none` returns nothing.

Suggestion: report the objects tree (and say "stored but not drawn" for idle slots). Add `clear_formula` / `new_scene` that resets unused slots to empty. Until that exists, an agent cannot make an honest preset card.

### 2. Any parameter whose name contains "color" is forced to be a color

`set_parameters` rejected the whole batch with `iteration_fog_color_1_maxiter must be #rrggbb or [r, g, b]` because the name contains `color`. Nothing in that call was applied, including `iteration_fog_enable` and `power`. The same trap will hit any `*_color_*_maxiter` or similar numeric field.

Suggestion: decide from the parameter type, not a substring of the name. If one field is invalid, apply the rest and return per-field errors (the success path already has an `errors` array; the color check bails before that).

### 3. `update_keyframe` replaces the stored camera

The description says passing `frames_per_keyframe` changes how many pictures lead to the next key. The implementation replaces the key with the camera as it is right now. Calling it on earlier keys, after the eye had moved, made every key identical, so the animation would have been a freeze. Two parallel updates also raced and collapsed the path.

Suggestion: if the only argument besides `index` is `frames_per_keyframe`, do not touch camera, target, or fov. Return the key as stored, and reject a second animation edit while one is in flight.

### 4. `frame_scene` trusts a bailout sphere

On `pseudo_kleinian` defaults, `frame_scene` reported `measured_radius` 100 and pulled the camera out to distance ~318. The picture was a distant blob. The real shape is a bubble cluster a few units across; a camera at `[0, -4.5, 1.6]` looking at `[0, 0, 0.2]` framed it. The tool's note still says "if the frame is still empty, set mandelbox_scale to -1.5", which is the wrong hint here.

Suggestion: when the measured radius hits the bailout or a hard cap, say so and do not move the camera that far. Offer "measurement looks like the escape sphere, try a closer camera" instead of the Mandelbox hint.

### 5. `load_preset` retargets the open file

After `load_preset`, `loaded_file` became the agent `.fract` under `~/mandelbulber/toolbar/agent/`. The result notes that MCP `save_settings` will not write it without `overwrite`, but the window's own Save now points at that card. A person who had `settings.fract` open can overwrite an agent preset, or save thinking they are saving their project.

Suggestion: load the preset into the session without changing the current filename. If the filename must change, say "the window's Save target is now this preset" in the first line of the result, not only inside `loaded_file`.

## P2 — Makes exploration slow or wrong

### 6. Primitives are half-specified

`add_object` accepts `radius`, and a sphere radius does take effect (a unioned sphere showed up, and `frame_scene` measured radius grew from ~1.22 to ~1.55). After that:

- `get_objects_tree` does not return radius, size, or primitive type. The node is only `type: primitive`.
- `update_object` has `scale`, not `radius`.
- `list_parameters` has no area for primitive parameters, so the radius cannot be read back.

A `boolean_sub` of a bulb and a sphere produced no obvious cavity from two viewpoints. One of those previews, with the eye close to the surface, took 1m 52s at 400px draft. The same boolean family from outside took 0.8s. There was no "the eye is inside / this will be slow" warning.

Suggestion: echo primitive kind, radius, and size on the tree. Let `update_object` set radius. If a preview runs much slower than the last one, include `camera_inside: true` or the hit label `inside` in the job note as soon as the first samples say so.

### 7. `describe_formula` is not a recipe

`where` and `typical_radius` are the useful part. The rest is the raw parameter window. Kaleidoscopic IFS returns nine direction blocks whose labels are all `x` or `y`, and `where` says the size is `formula_scale` when the control is `IFS_scale`. Pseudo Kleinian returns dozens of `transf_*` names with repeated labels (`offset`, `alpha`) and no "set these three first". Shared `power` is described as the Mandelbox exponent. It is not; `mandelbox_scale` is.

Suggestion: a `start_here` list of three to six controls with one sentence and a known-good value. Keep the full list behind `verbose: true`. Fix `where` to name the real parameter.

### 8. Search ranking and empty catalogs

- `list_formulas` query `mandelbulb` leads with Aexion and other summary matches. The formula named Mandelbulb is sixth.
- `list_examples` with an empty query returns 20 files from one collection (Graeme McLaren), not a spread of the catalog, and there is no collection filter.
- `list_parameters` query `fog` returns basic-fog descriptions pasted onto distance-fog and iteration-fog primitive fields. Query `background` returns fog parameters because the word appears in a tooltip. There is no `background_color` hit in the effects area.

Suggestion: rank exact name matches first, then prefix, then summary. Empty `list_examples` should return a short sample per collection plus a count. Parameter search should match name before description, and each description should belong to that parameter.

### 9. Status text does not match the job

- `render_preview` quality `better` still says "Draft preview from a copy" in `note`. `summary` correctly says `better`.
- A finished preview returned `message: "Starting rendering of image"` with `status: finished`.
- `render_animation` with `wait: false` returned `status: running` and `message: "Waiting in the agent queue"`.
- Completed 32-frame jobs reported `total time` of 0.2s to 1.5s. Single previews of the same scenes were 0.2s to 4s, and one boolean view was 112s. The frames are real and different, so the timer is not the wall clock of the job.

Suggestion: one `message` that matches `status`. Report elapsed wall time for the whole animation. Keep `note` in sync with `quality`.

### 10. `inspect_preset` does not show the card an agent can re-read

The text result for "Classic power-8 bulb" had formulas (the slot list), camera, fov, and resolution. It did not include power, scale, material, effects, or a thumbnail path. The description says the image is the toolbar thumbnail. If that image is only a side channel, a later turn cannot open it.

Suggestion: return `thumbnail` as a file path, plus the drawn tree, power or scale, and which effects are on.

### 11. `undo` does not say what it undid

The result is a full session summary. After `navigate_camera`, one `undo` returned the same camera numbers as before the call, with no `undone` field. It is not obvious whether the step failed or the summary is stale.

Suggestion: return `undone: "navigate_camera"` or `nothing_to_undo`, and the camera before and after.

## P3 — Polish

### 12. Orbit has no arc length

`set_flight_path` kind `orbit` is a full circle. That worked for these scenes, including the Mandelbox interior. A 30-degree look-around inside a corridor still has to be built from keyframes, which then hits issue 3. A `sweep` in degrees (default 360) would cover both.

### 13. Preview payloads are mostly the cell grid

A draft preview JSON is about 2,000 lines. The decision data is `image`, `message`, and a handful of ranked cells. The full grid on every call makes the image path easy to miss.

Suggestion: default to `best` plus the top five cells. `grid: full` for the rest.

### 14. No clean-slate tool

Starting a new look means deleting every node, then noticing the old slots are still in the file. A `new_scene` (one undo step, does not touch the user's file) that installs one Mandelbulb, default camera, and empty flight path would match how the guidance recipes are written.

### 15. What worked

These were enough to find and keep nine scenes without editing files:

- `get_guidance` recipes for the bulb, the Amazing Box scale, and hybrid sibling order.
- `frame_scene` on bulbs, the box, the hybrid, the IFS, the sponge, and the quaternion.
- `navigate_camera` move / orbit / look_at inside the Mandelbox.
- `set_material` gradient string and a second material on the primitive.
- `set_light` without a position (camera-attached).
- `render_preview` off the UI thread; the window stayed usable.
- `save_preset` / `list_presets` split, and `delete_preset` refusing user cards (not exercised; the refusal is documented and the user row was unchanged).
- `render_animation` `directory`, `width`/`height` with `long_side: 0`, `contact_sheet: true`, and `render_status` `wait: true` as the completion notice.
- `set_flight_path` kind `orbit` leaving keyframes alone.

`look_around` prefer `space` ranked sky outside the frame above an actual opening. It was less useful than `probe_view` / flying forward in the box.

## Scenes saved in this pass

Agent cards live in `~/mandelbulber/toolbar/agent/`. Previews are in `presets/previews/`. Each animation is 32 flight frames, 320×180, quality draft, under `presets/renders/<slug>/` with `contact_sheet.png`.

| Preset | Drawn formula | Look |
| --- | --- | --- |
| Classic power-8 bulb | Mandelbulb, power 8 | Warm gold bulb, dark ground, three-quarter view |
| Box arch interior | Mandelbox, scale −1.5 | Stone arch and passage, camera inside |
| Bulb-box hybrid | Mandelbulb power 8, then Mandelbox scale −1.5 ×3 | Bulb cut by rectangular box folds |
| Bulb and pearl | Boolean union of Mandelbulb power 8 and a sphere | Fractal bulb with a smooth pale sphere |
| Fog glow bulb | Mandelbulb, power 9 | Violet, coral, and gold iteration fog, strong glow |
| Copper IFS crystal | Kaleidoscopic IFS, abs on x/y/z, scale 2 | Copper crystal on black |
| Sandstone Menger | Menger sponge | Porous cube, sandstone and teal |
| Green quaternion | Quaternion, power 2 | Twisted organic shell, moss and gold |
| Kleinian bubble hall | Pseudo Kleinian | Glass bubbles under a blue vault |

Reported animation `total time` was under two seconds for every job. Treat that figure as unreliable until the timer is checked; the frame files and contact sheets are complete (288 frames).
